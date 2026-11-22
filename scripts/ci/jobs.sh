# shellcheck shell=bash
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
# The scratch a job needs, in MB (scripts/scratch.tsv, measured by the runs). A job that is not in it counts JOB_MB.
declare -A EST_MB=()
while IFS=$'\t' read -r job mb; do [[ $job == '#'* || -z $job ]] || EST_MB[$job]=$mb; done <"$REPO/scripts/scratch.tsv"
JOB_MB=100

# build_checks: the two checks of the build tree. A shard that installs a staged root (SIFT_PREBUILT_ROOT, scripts/ci-build.sh) has no objects and no headers to run them on: the job
# reports what the build stage recorded for them
build_checks() {
	if [[ -n ${SIFT_PREBUILT_ROOT:-} ]]; then
		job isa-baseline isa-baseline 0 "$S/ci-build.sh" --check isa-baseline
		job tools-build tools-build 0 "$S/ci-build.sh" --check tools-build
	else
		job isa-baseline isa-baseline 0 "$S/isa_check.sh"
		job tools-build tools-build 0 "$S/check-tools-build.sh"
	fi
	format_checks
}
# The format compatibility tiers (the v5 formats stay readable and writable beside the default 2.0; the build refuses nothing of Format 1):
#   1. every regression test runs on the default format (2.0): installcheck-*;
#   2. compat_v1_* (test/sql) build Format 1 with SET sift.test_create_format1 = on, a setting of the testing flavour only: scripts/check-test-settings.sh keeps it out of the release
#      library and scripts/check-testing-regress.sh keeps the tests that set it in TESTING_REGRESS (Makefile: compat_v1_%), so release-library-regress never runs them;
#   3. tap-013_upgrade_v5_to_v6 and the tap-shell upgrade tests of the =compat group (scripts/areas.tsv) upgrade a golden v5 cluster by binary swap, ALTER EXTENSION UPDATE and pg_upgrade;
#   4. format-isolation: no source of one format includes the other's (scripts/check-format-isolation.sh, static, no build; queued once the script exists).
format_checks() { [[ ! -x $S/check-format-isolation.sh ]] || job format-isolation format-isolation 0 "$S/check-format-isolation.sh"; }
tap_perl() { local f; for f in "$@"; do job "tap-$f" tap 0 "$S/tap.sh" "t/$f.pl"; done; }
ISOLATION_SLICES=4
isolation_slices() { local k; for (( k = 1; k <= ISOLATION_SLICES; k++ )); do job "isolation-$k" isolation 1 "$S/isolation.sh" --slice "$k/$ISOLATION_SLICES"; done; }
# A shell test that says `# tap: pooled` (test/t/lib.bash: SIFT_POOLED_CLUSTER) runs in a database of the lane's cluster, not in a server of its own
declare -A POOLED_TAP=()
while IFS= read -r f; do f=${f##*/}; POOLED_TAP[${f%.sh}]=1; done < <(grep -lx '# tap: pooled' "$REPO"/test/t/*.sh)
tap_shell() {
	local f
	for f in "$@"; do
		if [[ -v POOLED_TAP[$f] ]]; then job "tap-shell-$f" tap-shell 1 env SIFT_POOLED_CLUSTER=1 "$S/pgtest.sh" tap "$f"
		else job "tap-shell-$f" tap-shell 0 "$S/pgtest.sh" tap "$f"; fi
	done
}
installcheck() { local t; for t in "$@"; do job "installcheck-$t" installcheck 1 "$S/pgtest.sh" regress "$t"; done; }
# The regression tests are cut in two buckets. Solo (regress_solo): a job of its own, a test that changes what the cluster holds beyond its database (the roles, the
# extension recreated under its own name) and the slow ones (SLOW_REGRESS, queued by the plan before the batches). Batched: everything else, in jobs of about
# REGRESS_BATCH_SECS of the measured seconds of their tests (scripts/regress-times.tsv; REGRESS_TEST_SECS for a test it does not know), packed longest test first into the lightest
# batch, so no batch is a straggler of its alphabet. Each job is one pg_regress run in one database with the leak and leftover checks of the batch (scripts/pgtest.sh regress; a
# failure is cut in halves to name the test). A VACUUM, a sift_table or a sift_col index is no reason to run alone: the clusters have no autovacuum, a test ends its backends
# before the next one starts, and what a test leaves behind is a failure the run names. The tests do not run side by side in one pg_regress schedule: each creates the
# extension, which one database holds once, and they share the names of their tables; the lanes of the pool run the batches side by side, each in a database of its own.
# installcheck_batches TEST...: the solo tests as jobs, the others packed in batches (regress_pack); W_BATCH holds the lane seconds each batch is expected to take
installcheck_batches() {
	local i=0 t load names b sel=()
	for t in "$@"; do
		[[ installcheck-$t =~ ${SIFT_CI_AFFECTED:-.} ]] || continue
		[[ -z ${SIFT_CI_ASSERT:-} || ! installcheck-$t =~ $ASSERT_SKIP ]] || continue
		if regress_solo "$t"; then installcheck "$t"; else sel+=("$t"); fi
	done
	(( ${#sel[@]} )) || return 0
	[[ -z ${SIFT_CI_ASSERT:-} ]] || REGRESS_BATCH_SECS=$(regress_lane_secs "${sel[@]}")
	while read -r load names; do
		read -ra b <<<"$names"
		job "installcheck-batch$((++i))" installcheck 1 "$S/pgtest.sh" regress "${b[@]}"
		W_BATCH[installcheck-batch$i]=$(batch_weight "$load")
	done < <(regress_pack "${sel[@]}")
}
