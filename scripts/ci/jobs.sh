# The job queue: job(), the helpers that queue a family of jobs, the scratch estimate of each job (EST_MB) and the regression batches.
# A job keeps its first place in the queue; SIFT_CI_JOBS and SIFT_CI_AFFECTED filter them as they are queued. Sourced by scripts/ci.sh; not a script of its own.
# ---------- the jobs ----------
# job ID SUITE CLUSTER CMD...: queued in the order of the calls, so the longest come first (a job queued twice keeps its first
# place). CLUSTER=1 uses the cluster of the lane (the job names its own databases); the others get a directory of their own and
# the port block of the lane.
declare -A J_SUITE J_CLUSTER J_CMD J_AFTER
J_ID=() SUITES=()
job() {
	local id=$1
	[[ -z ${SIFT_CI_JOBS:-} || $id =~ $SIFT_CI_JOBS ]] && [[ ! -v J_SUITE[$id] ]] || return 0   # a job keeps its first place in the queue
	[[ $id == installcheck-batch* || $id =~ ${SIFT_CI_AFFECTED:-.} ]] || return 0   # affected: the batches are cut from the tests that match (installcheck_batches)
	J_ID+=("$id")
	J_SUITE[$id]=$2 J_CLUSTER[$id]=$3
	[[ " ${SUITES[*]-} " == *" $2 "* ]] || SUITES+=("$2")
	shift 3
	printf -v "J_CMD[$id]" '%q ' "$@"
}
# flavor_job ID SUITE CMD...: a job that installs a library flavor of its own (scripts/pgtest.sh install: hardened, release). On PostgreSQL
# 18 the install is staged under the job's root and it runs in the pool. Before 18 it overwrites the server's own library, which the
# workers of the lanes load, and the lanes would test the wrong flavor or fail to load it: it runs after the pool (--selftest checks the plan)
flavor_job() {
	job "$1" "$2" 0 "${@:3}"
	(( PG_MAJOR >= 18 )) || [[ ! -v J_SUITE[$1] ]] || J_AFTER[$1]=1
}
named() { local n; for n in "$@"; do echo "$n"; done; }
# rest DIR SUFFIX NAME...: the files test/DIR/*SUFFIX that are not named, so that a new test is queued without editing the lists
rest() {
	local n
	for n in $(basename -a -s "$2" "$REPO/test/$1"/*"$2"); do [[ " ${*:3} " == *" $n "* ]] || echo "$n"; done
}
# The scratch a job needs, in MB: the peak of its lane directory with the cluster of the lane in it (SIFT_CI_STATS=1 lists it per job). The
# jobs below grow the store (the churn of the store tests, WAL of the WAL tests, spill of a large build) and are the only ones above JOB_MB.
declare -A EST_MB=([tap-013_tbl_bigtx]=820 [tap-014_tbl_writepath]=1130 [installcheck-tbl_writepath]=500 [tap-shell-old_lib_refusal]=470 [tap-shell-scaling_slope]=1400 [installcheck-tbl_bigtx]=700 [tap-004_wal_consistency]=690 [tap-shell-store_replica]=500 [tap-006_truncate]=550 [tap-010_tbl_settings]=420
	[tap-006_tbl]=380 [tap-002_standby]=380 [tap-shell-maint_memory]=330 [tap-shell-extension_lifecycle]=360 [installcheck-batch5]=260 [installcheck-index_build_postings]=230
	[tap-shell-replication]=200 [tap-shell-store_concurrency]=200 [tap-shell-index_build_crash]=180
	[tap-shell-store_crash]=170 [tap-001_crash]=150 [installcheck-index_build_thresholds]=140
	[release-library-regress]=900 [installcheck-agg_group_spill_fuzz]=800 [tap-shell-col_run_heads_replica]=480 [installcheck-tbl_cap]=480 [installcheck-col_approx]=350)
JOB_MB=70

tap_perl() { local f; for f in "$@"; do job "tap-$f" tap 0 "$S/tap.sh" "t/$f.pl"; done; }
tap_shell() { local f; for f in "$@"; do job "tap-shell-$f" tap-shell 0 "$S/pgtest.sh" tap "$f"; done; }
installcheck() { local t; for t in "$@"; do job "installcheck-$t" installcheck 1 "$S/pgtest.sh" regress "$t"; done; }
# installcheck_batches N TEST...: the short tests, N to a pg_regress run in one database like make installcheck runs them: a run costs 0.3 s of
# CPU and lane time before its first statement, which is more than most of these tests need. The tests that VACUUM keep a database of
# their own: VACUUM removes (and a sift_table seals) only the rows older than every snapshot of the database, and an autovacuum worker on
# the tables an earlier test left behind holds one
installcheck_batches() {
	local n=$1 i=0 b=() t f
	local -A stateful=()   # the tests that VACUUM or use a sift_table, found in one grep (one a test was 330 forks of every plan)
	while IFS= read -r f; do f=${f##*/}; stateful[${f%.sql}]=1; done < <(grep -liE 'using sift_table|\<vacuum\>' "$REPO"/test/sql/*.sql)
	shift
	for t in "$@"; do
		[[ installcheck-$t =~ ${SIFT_CI_AFFECTED:-.} ]] || continue
		[[ -z ${SIFT_CI_ASSERT:-} || ! installcheck-$t =~ $ASSERT_SKIP ]] || continue
		if [[ -v stateful[$t] ]]; then installcheck "$t"; continue; fi
		b+=("$t")
		(( ${#b[@]} < n )) || { job "installcheck-batch$((++i))" installcheck 1 "$S/pgtest.sh" regress "${b[@]}"; b=(); }
	done
	(( ${#b[@]} == 0 )) || job "installcheck-batch$((++i))" installcheck 1 "$S/pgtest.sh" regress "${b[@]}"
}
