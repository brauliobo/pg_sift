# shellcheck shell=bash
# The tier settings of a run: plan or run, cores, PostgreSQL major, lanes, scratch budget and cap, and the timeout of a job.
# Everything below reads MODE, PLAN, LANES, BUDGET_MB, MAX_MB and JOB_TIMEOUT from here. Sourced by scripts/ci.sh; not a script of its own.
PLAN=
[[ $MODE != --plan && $MODE != --toolchain && $MODE != --history ]] || { PLAN=${MODE#--}; MODE=${2:-quick}; }
[[ $MODE == quick ]] || { usage; exit 1; }
# SIFT_FORMAT: the format the run creates (scripts/pgtest.sh): 2 (default) or 1, which runs the same jobs on Format 1.0 but those that the Format 2.0 run covers or that do not depend on the
# format (scripts/ci/format1-skip.txt, and every job that builds an older release). SKIP_RE: the jobs the run never queues; SIFT_CI_SKIP, a regex on job names, adds to it
FORMAT=${SIFT_FORMAT:-2}
[[ $FORMAT == [12] ]] || { echo "ci: SIFT_FORMAT is 2 (default) or 1, not '$FORMAT'" >&2; exit 2; }
export SIFT_FORMAT=$FORMAT
SKIP_RE=${SIFT_CI_SKIP:-}
[[ $FORMAT == 2 ]] || SKIP_RE+=${SKIP_RE:+|}$(grep -vE '^[[:space:]]*(#|$)' "$REPO/scripts/ci/format1-skip.txt" | paste -sd'|' -)
[[ -n $PLAN ]] || renice -n 19 $$ >/dev/null
S=$REPO/scripts
CORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
[[ $(pg_config --version 2>/dev/null) =~ ^PostgreSQL\ ([0-9]+) ]] && PG_MAJOR=${BASH_REMATCH[1]} || PG_MAJOR=0
PG_MAJOR=${SIFT_CI_PG_MAJOR:-$PG_MAJOR}
[[ -n ${FLAVOUR:-} ]] || if [[ $(uname -m) == @(aarch64|arm64) ]]; then FLAVOUR=arm; else FLAVOUR=$PG_MAJOR; fi   # the column of the weights this runner is cut by
LANES=${SIFT_LANES:-$(( CORES < 8 ? CORES : 8 ))}
# BUDGET_MB, the scratch the running jobs may need together (estimates below): 1800, CI_BUDGET_MB. The cap, CI_MAX_MB, stops the run (and fails the shard) when the
# directory really exceeds it: 3072, the budget of a shard (scripts/budgets.tsv); the estimates (EST_MB) and the 400 MB that the install, the templates and the clusters of the lanes
# hold keep it under. A hosted shard (.github/workflows/ci-pg.yml, ci.yml cassert) says 3600 and 5120: at 1800 its regression batches (650 MB each) ran two at a time on 4 cores, and its
# scratch is in a /dev/shm of 7.8 GB
export SIFT_QUICK=1; BUDGET_MB=${CI_BUDGET_MB:-1800}; MAX_MB=${CI_MAX_MB:-3072}
# CI_JOB_TIMEOUT: seconds a job may run before it is killed (900; the slowest job takes 283 s in hosted run 42084189989), so that a
# stuck test fails with the end of its log instead of holding the runner until the limit of the workflow. Not enforced where there is no timeout(1) (macOS)
JOB_TIMEOUT=()
for tm in timeout gtimeout; do command -v $tm >/dev/null && { JOB_TIMEOUT=($tm -k 20 "${CI_JOB_TIMEOUT:-900}"); break; }; done
