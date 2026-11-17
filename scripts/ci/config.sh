# The tier settings of a run: plan or run, cores, PostgreSQL major, lanes, scratch budget and cap, and the timeout of a job.
# Everything below reads MODE, PLAN, LANES, BUDGET_MB, MAX_MB and JOB_TIMEOUT from here. Sourced by scripts/ci.sh; not a script of its own.
PLAN=
[[ $MODE != --plan ]] || { PLAN=1; MODE=${2:-quick}; }
[[ $MODE == quick || $MODE == full ]] || { usage; exit 1; }
[[ -n $PLAN ]] || renice -n 19 $$ >/dev/null
S=$REPO/scripts
CORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
[[ $(pg_config --version 2>/dev/null) =~ ^PostgreSQL\ ([0-9]+) ]] && PG_MAJOR=${BASH_REMATCH[1]} || PG_MAJOR=0
PG_MAJOR=${SIFT_CI_PG_MAJOR:-$PG_MAJOR}
LANES=${SIFT_LANES:-$(( CORES < 8 ? CORES : 8 ))}
# BUDGET_MB, the scratch the running jobs may need together (estimates below): quick 2000, full 2000, CI_BUDGET_MB. The cap, CI_MAX_MB, stops the run (and fails the shard) when the
# directory really exceeds it: quick 3072, the budget of a shard (scripts/budgets.tsv); the estimates (EST_MB) and the 400 MB that the install, the templates and the clusters of the lanes
# hold keep it under; full 8192 up to four lanes, 512 more for each lane above four
if [[ $MODE == full ]]; then
	export SANITIZE=1; BUDGET_MB=${CI_BUDGET_MB:-2000}; MAX_MB=${CI_MAX_MB:-$(( 8192 + 512 * (LANES > 4 ? LANES - 4 : 0) ))}; unset SIFT_QUICK
else
	export SIFT_QUICK=1; BUDGET_MB=${CI_BUDGET_MB:-2000}; MAX_MB=${CI_MAX_MB:-3072}
fi
# CI_JOB_TIMEOUT: seconds a job may run before it is killed (quick 900, full 3600; the slowest job takes 283 and 2016 s in hosted runs 42084189989 and 42064806680), so that a
# stuck test fails with the end of its log instead of holding the runner until the limit of the workflow. Not enforced where there is no timeout(1) (macOS)
JOB_TIMEOUT=()
for tm in timeout gtimeout; do command -v $tm >/dev/null && { JOB_TIMEOUT=($tm -k 20 "${CI_JOB_TIMEOUT:-$([[ $MODE == full ]] && echo 3600 || echo 900)}"); break; }; done
