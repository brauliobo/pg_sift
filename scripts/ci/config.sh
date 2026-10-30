# The tier settings of a run: plan or run, cores, PostgreSQL major, lanes, scratch budget and cap, and the timeout of a job.
# Everything below reads MODE, PLAN, LANES, BUDGET_MB, MAX_MB and JOB_TIMEOUT from here. Sourced by scripts/ci.sh; not a script of its own.
PLAN=
[[ $MODE != --plan ]] || { PLAN=1; MODE=${2:-quick}; }
[[ $MODE == quick || $MODE == full ]] || { sed -n '2,27p' "$0"; exit 1; }
[[ -n $PLAN ]] || renice -n 19 $$ >/dev/null
S=$REPO/scripts
CORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
[[ $(pg_config --version 2>/dev/null) =~ ^PostgreSQL\ ([0-9]+) ]] && PG_MAJOR=${BASH_REMATCH[1]} || PG_MAJOR=0
PG_MAJOR=${SIFT_CI_PG_MAJOR:-$PG_MAJOR}
LANES=${SIFT_LANES:-$(( CORES < 8 ? CORES : 8 ))}
# BUDGET_MB, the scratch the running jobs may need together (estimates below): quick 1200, full 2000, CI_BUDGET_MB. The cap, CI_MAX_MB, stops the
# run when the directory really exceeds it: quick 4096 (the quick tier peaks at 2.1 to 2.6 GB on PostgreSQL 18 shard 2 and 17 shard 1, runs 40145861942 and 40149214243), full 8192 up to four lanes, 512 more for each lane above
if [[ $MODE == full ]]; then
	export SANITIZE=1; BUDGET_MB=${CI_BUDGET_MB:-2000}; MAX_MB=${CI_MAX_MB:-$(( 8192 + 512 * (LANES > 4 ? LANES - 4 : 0) ))}; unset SIFT_QUICK
else
	export SIFT_QUICK=1; BUDGET_MB=${CI_BUDGET_MB:-1200}; MAX_MB=${CI_MAX_MB:-4096}
fi
# CI_JOB_TIMEOUT: seconds a job may run before it is killed (quick 3000, full 7200; the slowest job of a shard takes 2100 and 4700 on a loaded runner), so that a
# stuck test fails with the end of its log instead of holding the runner until the limit of the workflow. Not enforced where there is no timeout(1) (macOS)
JOB_TIMEOUT=()
for tm in timeout gtimeout; do command -v $tm >/dev/null && { JOB_TIMEOUT=($tm -k 20 "${CI_JOB_TIMEOUT:-$([[ $MODE == full ]] && echo 7200 || echo 3000)}"); break; }; done
