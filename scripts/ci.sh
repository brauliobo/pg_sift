#!/usr/bin/env bash
# Single entry point for the test suites, with a pass/fail/skip summary.
#
#   scripts/ci.sh [quick|full]        (make check | make check-full)
#   scripts/ci.sh affected [--list|--github] [BASE]   quick, only the jobs the diff against BASE (default origin/main, with the working tree) can break
#                                     (scripts/areas.tsv; --list prints them, --github the outputs of the workflow's `changes` job)
#   scripts/ci.sh --times [RESULTS]   the per-job wall and CPU seconds of a kept run (KEEP=1), longest first
#   scripts/ci.sh --plan [quick|full] the jobs and whether each runs in the pool or after it; --selftest checks that on every PostgreSQL major
#
# quick (default, about a minute on 8 free cores): non-ASan build, small corpora and short runs. installcheck (also against the
#   release library, without the test-only functions: scripts/release-library-check.sh), isolation (emulated and real), the Perl and shell TAP tests (sift only, fewer crash
#   iterations), a 2 500 document differential test and 8 of the 16 mutants of the harness (the
#   fast-to-detect ones), the differential test of the aggregate pushdown and its self-test (scripts/coldiff.sh, docs/AGG.md), the proof of
#   the coverage table, a 15 s soak, an 18 s column-index soak (scripts/colsoak.sh) and a
#   fuzz pass with shorter loops (scripts/fuzz.sh).
# full: ASan build and clusters, installcheck, the differential test at 5 000 documents, all 16
#   mutants, every isolation and TAP test (GIN control included), all fuzz targets, GIN-control
#   and sift soaks (600 s), the column-index soak (300 s) and the server sanitizer report.
# SANITIZE=1 makes quick run on the ASan build too.
#
# The suites are cut into jobs (a regression test, a TAP file, a differential test...) that a pool of lanes takes from one
# queue, longest first: SIFT_LANES lanes (default: the cores, at most 8), each with its own cluster (started when its first
# job needs one) and one shared install, at nice 19. A lane starts a job when the scratch estimated for the jobs running (EST_MB,
# JOB_MB) plus its own fits CI_BUDGET_MB (quick 1200, full 2000; the first job always runs), so the run stays at about 1 GB
# however many lanes there are. The scratch directory is $SIFT_CI_TMP (default $TMPDIR/sift-ci-<pid>, /tmp when unset and on macOS), removed
# when everything passed or the run was interrupted (INT, TERM; KEEP=1 keeps it; a failure keeps it for the logs, a day); a job that passed leaves it at once. On exit, and at the start of
# the next run (the directories of dead pids), every postmaster under the directory is stopped and the directory removed: no run leaves a cluster behind. The ports are
# SIFT_PORT (default 56130) and +10 per lane. A watchdog stops the run when the directory exceeds CI_MAX_MB (quick 4096; full
# 8192, 512 more per lane above four). SIFT_CI_STATS=1 prints the footprint of the run (peak by component, lane and job, and the
# memory of the postgres processes) and is how EST_MB is kept up to date. SIFT_CI_JOBS, a regex on the job names
# (installcheck-facets, tap-001_crash, tap-shell-store_crash, difftest-mutants, soak, fuzz...), runs just those (the regression tests of
# SLOW_REGRESS are jobs of their own, the others run twelve to a job, installcheck-batch1...: scripts/pgtest.sh regress NAME runs one).
# SIFT_CI_AFFECTED, a regex on the job names, keeps the jobs that match (`affected` sets it from scripts/areas.tsv); SIFT_CI_SHARD=K/N runs the Kth of N slices of the queue.
# `affected --focus [--list] [BASE]` narrows the regression tests of a change in src/ to the ones that execute the functions the diff touched (scripts/focus.tsv, made by
# scripts/focus-gen.sh with gcov): a function every test executes, or a change outside functions, runs the tests of the area as in the fast tier; the other jobs only run in CI.
# SIFT_CI_NO_HARNESS=1 skips the checks of the harness that shard 1 runs (the workflow sets it: its `lint` job runs them once for every run). Logs:
# <scratch>/logs/<suite>.log, and <job>.log for each job of a split suite. Stages whose sift code has not landed are skipped, not
# failed. Exit 1 if any suite fails.
#
# hardened (scripts/hardened.sh) builds the tree with SIFT_HARDEN=1 (docs/SAFETY.md: stack protector, FORTIFY_SOURCE=3, zero-initialized locals, trap-mode
# UBSan) in a private copy and runs the regression tests of the decoders on it (quick: six; full: all of them); span-test runs the unit tests of
# the bounds-checked reader (src/core/span.h) plain and under ASan+UBSan; static (full only, scripts/static.sh) runs clang-tidy, the gcc and
# clang analyzers and cppcheck where installed, and reports the findings that are not in scripts/static.baseline without failing the run.
#
# assert (scripts/assert.sh) and valgrind (scripts/valgrind.sh) are not part of quick or full: they run a PostgreSQL built from source with assertions and
# memory-context checking (and USE_VALGRIND) on the regression subset, plus two TAP tests for assert, when SIFT_CI_JOBS names them (SIFT_CI_JOBS=assert).
#
# The last stage, leakcheck, is the leak gate over everything the lanes ran (scripts/leakcheck.sh): the
# server logs of every cluster and node, the sanitizer and LeakSanitizer reports, and the cluster checks
# recorded when each one stopped. The harness checks (scripts/lint.sh, the span tests) run beside the lanes in shard 1; the leakcheck selftest in lint proves that the gate fails on each kind of leak.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }   # first: bash 3.2 (stock macOS) fails on the syntax below and exits 0
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
# a copy outside a checkout (REPO is then /) must stop, not run the harness on nothing, and a run that starts itself again and again must stop at some depth
[[ -f $REPO/scripts/ci.sh && -f $REPO/Makefile ]] || { echo "ci.sh runs from the scripts/ of a checkout, not from $(dirname "$0")" >&2; exit 2; }
export SIFT_CI_DEPTH=$(( ${SIFT_CI_DEPTH:-0} + 1 ))
(( SIFT_CI_DEPTH <= 8 )) || { echo "ci.sh started from itself $SIFT_CI_DEPTH levels deep, stopping" >&2; exit 2; }
MAIN=$$
MODE=${1:-quick}
# --times [RESULTS]: the per-job table of a kept run (KEEP=1; default the newest $TMPDIR/sift-ci-*), longest first: job, wall seconds, CPU
# seconds (client plus server processes), lane, status
# --shards PG [deep]: the runners of the full tier on that PostgreSQL major, the one place the workflow and the selftest read (the weights below balance the jobs over them)
if [[ $MODE == --shards ]]; then
	if [[ -n ${3:-} ]]; then (( ${2:?} == 17 )) && echo 5 || echo 9; else echo 4; fi
	exit
fi
if [[ $MODE == --times ]]; then
	R=${2:-$(ls -td "${TMPDIR:-/tmp}"/sift-ci-*/logs/results 2>/dev/null | head -1)}
	sort -k4,4nr "$R" | awk 'BEGIN { printf "%-44s %7s %7s  %-6s %s\n", "job", "wall_s", "cpu_s", "lane", "status" }
		{ printf "%-44s %7d %7.1f  %-6s %s\n", $1, $4, $9, $6, $3; w += $4; c += $9 } END { printf "%-44s %7d %7.1f\n", "total", w, c }'
	exit
fi
# the rest of the run, in the order it needs: each part is a piece of this script, sourced into this shell (scripts/ci/NAME.sh)
source "$REPO/scripts/ci/affected.sh"
source "$REPO/scripts/ci/weights.sh"
source "$REPO/scripts/ci/procs.sh"
source "$REPO/scripts/ci/selftest.sh"
source "$REPO/scripts/ci/config.sh"
source "$REPO/scripts/ci/jobs.sh"
source "$REPO/scripts/ci/plan.sh"
source "$REPO/scripts/ci/shards.sh"
source "$REPO/scripts/ci/claim.sh"
source "$REPO/scripts/ci/lane.sh"
source "$REPO/scripts/ci/scratch.sh"
source "$REPO/scripts/ci/run.sh"
source "$REPO/scripts/ci/summary.sh"
