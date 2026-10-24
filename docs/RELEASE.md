# Release

GitHub CI is the gate of a release. `scripts/release.sh` does not run the test matrix again: it reuses the green CI run of the commit, adds what CI
cannot give, and ships.

```
scripts/release.sh VERSION --dry-run   everything on a scratch clone: nothing is pushed, tagged or published
scripts/release.sh VERSION             the release
scripts/release.sh VERSION --resume    continue a stopped release at the step that failed
```

Before it: write `docs/releases/vVERSION.md` from `docs/releases/TEMPLATE.md`, have the notes of the release under `## Unreleased` in
`CHANGELOG.md`, `pg_sift.control` at the version of the extension SQL (a patch release keeps it), and push `main`. When the last code commit is pushed,
`gh workflow run ci.yml --ref main` starts the full run of that commit (about 8 minutes), so that the release finds it done.
After a minor release replace `scripts/options-released.tsv` with `scripts/gen-options.sh --snapshot` of the release, so that the options tables stop marking its defaults as new or changed.

## What it does

| Step | |
|---|---|
| preflight | clean tree on `main`, equal to `origin/main`; notes, CHANGELOG, control, SQL scripts (`sql/pg_sift--PREV--NEW.sql` for a new minor) in place; `scripts/check-bench-cases.sh --strict` (`bench/bench cases --check --strict`): every speed line of `## Unreleased` has a case in `bench/cases.tsv` or a reasoned line in `scripts/bench-cases.allow` (a dry run only prints the count of the lines left) |
| `ci` | a green run of the workflow `ci` marked "Full scope" (every job of the quick tier ran; the run's `changes` job has the step), on any ref but a pull request (main, a dispatch of a branch, the nightly), whose commit has no difference from HEAD in the build inputs (`src test scripts Makefile pg_sift.control sql/pg_sift--*.sql sqlite mariadb`, except `*.md`, `VERSION` files, the release scripts and `scripts/bench-cases.allow`): a rebase merge, a revert or a docs commit keeps the evidence. A run in progress is waited for; with none, the full run is dispatched |
| `upgrade` | the previous tag's library writes a BM25 index, a `sift_col` index and a `sift_table` in a cluster; this tree's files replace it, `ALTER EXTENSION pg_sift UPDATE`, answers equal, `sift.verify`, `sift.col_verify`, `REINDEX`; then, with `sift.col_format_minor` at its lowest value, the previous library reads what this tree wrote |
| `package` | `make install DESTDIR` (production build): every extension file and the library in place, no test-only file, `packaging/smoke.sql` from the first and from the last version in a cluster that sees only the installed files |
| `bump` | `CHANGELOG.md` and the CHANGELOG and `VERSION` of the ports, `PKGBUILD`, README badge, install lines: one commit, after a check that every place agrees |
| publish | `main` and the tag in one push; the workflow `release` (the `.deb` and `.rpm` in parallel, the SQLite and MariaDB tarballs); the downloaded assets against `SHA256SUMS` and their names; `scripts/release-aur.sh` |

`ci`, `upgrade` and `package` run at once. The steps that passed are in `~/.local/state/pg_sift-release/VERSION/manifest` and the logs in `logs/` there.
The AUR step needs the key: a key loaded in your ssh-agent, or `AUR_SSH_KEY=PATH scripts/release.sh VERSION` (the passphrase is asked through your `SSH_ASKPASS`).
Scratch is under `~/.cache` (`RELEASE_TMP`), the run in a scope of 2 cores and 12 GB, ports from `SIFT_PORT` (56210) and the 10th after it.

## Homebrew tap

The tap is this source repository: `Formula/pg_sift.rb` is maintained alongside the extension.
Install with `brew tap brauliobo/pg_sift https://github.com/brauliobo/pg_sift.git`, then `brew install --HEAD brauliobo/pg_sift/pg_sift`.
The HEAD-only formula builds `main` and supports macOS arm64. No tag or release is created by the tap.

To add a stable formula, create an immutable release tag, then add `url "https://github.com/brauliobo/pg_sift/archive/refs/tags/vVERSION.tar.gz"` and its downloaded source tarball's
`sha256` to `Formula/pg_sift.rb` before `license` in a subsequent commit. Do not include a tag’s own tarball checksum in that tag: the formula would change the archive being checksummed. Keep HEAD tracking `main`. For later releases, use
`brew bump-formula-pr --url=https://github.com/brauliobo/pg_sift/archive/refs/tags/vVERSION.tar.gz brauliobo/pg_sift/pg_sift`.
Review the URL and checksum, run `brew style` and `brew audit --strict --online`, then install from the GitHub tap and run `brew test` on macOS for both PostgreSQL versions
before publishing the formula update. The formula reads the installed control version when testing the upgrade.

## What CI runs

A push runs the jobs its diff can break; the map is `scripts/areas.tsv`, shared by the workflow and `scripts/ci.sh affected`. The base of a push is the last green run of
`main`, so a red or cancelled push is not forgotten by the next one. A change of `src/core`, the build or the harness runs everything.

| Change | Runs |
|---|---|
| docs, bench, packaging, other workflows | lint (syntax of the scripts, portable core, `scripts/ci.sh --selftest`, actionlint); packaging also the release dry run |
| `src/bm25` | the BM25 jobs, the decoders, the search callers, SQLite, MariaDB, replication |
| `src/col`, `src/agg` | the column, aggregate and table jobs and the common ones (production build, coverage, isolation) |
| `src/table` | the table jobs and the common ones |
| a test file | that test |
| `Formula/`, the build or the harness (and the nightly run) | also the `macos` job: the formula (`brew install --HEAD`, `brew test`) on PostgreSQL 17 and 18, two regression tests under brew bash 5, and `make check` under the stock bash 3.2, which must fail with a message |

PostgreSQL 17 and 18 run the quick tier, in two shards when more than 40 jobs are picked (`SIFT_CI_SHARD=K/N`: every Nth job of the longest-first queue;
PostgreSQL 17 has a third shard for the jobs that install a library flavor, which cannot share a server with the pool).
PostgreSQL 19, built from `REL_19_STABLE` and cached (`.github/actions/pg19`, kept warm by `pg19-cache.yml`), runs the same jobs in the same shards and fails the run like 17 and 18.
19 is a supported major: the source in `src/core/compat.h` carries its differences, and a red 19 job blocks a release like any other.

Following the 19 betas and release candidates needs no change in the repository: the action builds the head of `REL_19_STABLE` (`scripts/pg19-build.sh`: assertions on, ICU, the TAP and contrib modules the tests use).
`pg19-cache.yml` runs on every push that touches the recipe and every six hours, and stores that build in the cache of `main`; a pull request run restores the newest build of the same recipe,
so it never waits for a compile and PostgreSQL 19 is at most six hours behind its branch. A new tag (`REL_19_RC2`, the final release) is on the branch already. Bump nothing for those.
What does need a bump: the cache key is the hash of `scripts/pg19-build.sh`, so a change of the configure flags rebuilds on its own; when 19.x is packaged by PGDG, move the job to the packages and drop
the git build; when 20 opens, change the default `ref` of the action to `master` (or `REL_20_STABLE` once it exists) and rename the job. To try a build by hand:
`scripts/pg19-build.sh /var/tmp/pg19 <commit>`, then `PATH=/var/tmp/pg19/bin:$PATH scripts/pgtest.sh install`.
`scripts/ci.sh --selftest` fails when a tracked file or a test is in no row of the map. Not per push, on the nightly run and on `gh workflow run ci.yml -f heavy=true`:
the full tier (`make check-full`: ASan, soaks and fuzz runs, all mutants) on 17 and 18, in a lean size (a job fits 45 minutes, the run 75: scripts/ci.sh, `lean_or_deep`) and, with the nightly run or `deep`, a deep one (long soaks, the whole differential corpus). `assert.yml`
(assertions and valgrind, weekly) and `coverity.yml` stay as they were. The benchmarks are a separate step, not part of a release: `bench/bench refresh` measures the numbers of the README again (bench/README.md, "Keeping the numbers fresh"), and
`scripts/check-bench-fresh.sh` (lint, and `release.sh`) fails when a generated block differs from `docs/results` and warns when the results are from another MAJOR.MINOR than `pg_sift.control`:
a release that prints that warning has benchmark numbers of an older version in its README, so run `bench/bench refresh` first or say so in the notes.
The shards of the full tier are cut by weight (`JOB_WEIGHT` in scripts/ci.sh: the seconds of a lane of each job of 90 s and more, measured on PostgreSQL 18 in the lean run 40097165396, 2026-10-24): by position the three PostgreSQL 18 shards were 98, 118 and 164 lane-minutes (the longest job 45 min); by weight they are within 15% of each other (`ci.sh --selftest` checks it). The deep tier (nightly) has its own weights for the jobs it makes larger (`JOB_WEIGHT_DEEP`, run 40079220375) and is cut finer so that no job passes the hour: `difftest-col` and `difftest-col-tbl` in 8 slices instead of 4, PostgreSQL 18 in 4 shards and 17 in 9 (8 of the pool and the flavor jobs), the full entries first in the matrix. Re-measure the table after a change of the sizes below.

The lean full tier runs everything the deep one runs, with these differences (SIFT_CI_DEEP=1, the nightly run or the `deep` input of a dispatch, has the larger side):
- `difftest-col`, `difftest-col-tbl`: each scenario state (insert, update, delete, abort, vacuum, hot, recycle, held, maint) meets a third of the scenario queries, rotating, instead of all of them; on PostgreSQL 17 only every other fixture (slices 1 and 2 of 4, in name order) runs, the other half runs on 18 (deep: all, on both).
- `difftest` 4000 documents, 100 queries, 3 rounds (deep 5000, 150, 4); `difftest-mutants` 4000 documents and 1 round, all 16 mutants (deep 8000, 2); `difftest-nopos` 2500 documents, 40 queries, 1 round (deep 3000, 60, 2); `difftest-fuzz` seeds 1-6 with 100 queries (deep 1-12, 150); `fuzz` scale 4 (deep 10); `soak` 180 s, `soak-gin` 60 s, `soak-col` 120 s (deep 600, 120, 300).
- Not cut: the regression tests (`installcheck`), the TAP tests, isolation, `hardened`, `coverage`, `production-regress`, `plan-gate`, the leak gate and the sanitizer reports, on every major.

`scripts/check-bench-cases.sh` (lint) fails when a CHANGELOG line a change adds under `## Unreleased` claims a speed and no case of `bench/cases.tsv` measures it, or a case feeds no block or has no row in `docs/results/cases.tsv`; `scripts/check-bench-cases.sh --strict` applies it to every line of Unreleased and lists the optimizations nobody measured: `release.sh` runs it in its preflight and stops while a line is left, so the count of lines it prints is the number to bring to zero (a case, or a line of `scripts/bench-cases.allow` with the reason a number cannot be measured); `bench/bench cases --docs --rows 1000000` gives the published numbers (bench/README.md, "Cases").
Job times and why the quick jobs have their size: [TEST-TIMES.md](TEST-TIMES.md).
