#!/usr/bin/env bash
# Single entry point for the test suites, with a pass/fail/skip summary.
#
#   scripts/ci.sh [quick|full]        (make check | make check-full)
#   scripts/ci.sh affected [--list|--github] [BASE]   quick, only the jobs the diff against BASE (default origin/main, with the working tree) can break
#                                     (scripts/areas.tsv; --list prints them, --github the outputs of the workflow's `changes` job)
#   scripts/ci.sh --times [RESULTS]   the per-job wall and CPU seconds of a kept run (KEEP=1), longest first (docs/TEST-TIMES.md)
#   scripts/ci.sh --plan [quick|full] the jobs and whether each runs in the pool or after it; --selftest checks that on every PostgreSQL major
#
# quick (default, about a minute on 8 free cores): non-ASan build, small corpora and short runs. installcheck (also against the
#   production library, without the test-only functions: scripts/production-check.sh), isolation (emulated and real), the Perl and shell TAP tests (sift only, fewer crash
#   iterations), a 2 500 document differential test and 8 of the 16 mutants of the harness (the
#   fast-to-detect ones), the differential test of the aggregate pushdown and its self-test (scripts/coldiff.sh, docs/AGG.md), the proof of
#   the coverage table (scripts/coverage.sh, docs/COVERAGE.md: about 10 s), a 15 s soak, an 18 s column-index soak (scripts/colsoak.sh) and a
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
# when everything passed (KEEP=1 keeps it; a failure keeps it for the logs); a job that passed leaves it at once. The ports are
# SIFT_PORT (default 56130) and +10 per lane. A watchdog stops the run when the directory exceeds CI_MAX_MB (twice the budget; full
# 8192, 512 more per lane above four). SIFT_CI_STATS=1 prints the footprint of the run (peak by component, lane and job, and the
# memory of the postgres processes) and is how EST_MB is kept up to date. SIFT_CI_JOBS, a regex on the job names
# (installcheck-facets, tap-001_crash, tap-shell-store_crash, difftest-mutants, soak, fuzz...), runs just those (the regression tests of
# SLOW_REGRESS are jobs of their own, the others run twelve to a job, installcheck-batch1...: scripts/pgtest.sh regress NAME runs one).
# SIFT_CI_AFFECTED, a regex on the job names, keeps the jobs that match (`affected` sets it from scripts/areas.tsv); SIFT_CI_SHARD=K/N runs the Kth of N slices of the queue.
# SIFT_CI_NO_HARNESS=1 skips the checks of the harness that shard 1 runs (the pull request tier: lint runs three of them). Logs:
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
# recorded when each one stopped. leakcheck-selftest first proves that the gate fails on each kind of leak.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }   # first: bash 3.2 (stock macOS) fails on the syntax below and exits 0
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
MODE=${1:-quick}
# --times [RESULTS]: the per-job table of a kept run (KEEP=1; default the newest $TMPDIR/sift-ci-*), longest first: job, wall seconds, CPU
# seconds (client plus server processes), lane, status (docs/TEST-TIMES.md)
if [[ $MODE == --times ]]; then
	R=${2:-$(ls -td "${TMPDIR:-/tmp}"/sift-ci-*/logs/results 2>/dev/null | head -1)}
	sort -k4,4nr "$R" | awk 'BEGIN { printf "%-44s %7s %7s  %-6s %s\n", "job", "wall_s", "cpu_s", "lane", "status" }
		{ printf "%-44s %7d %7.1f  %-6s %s\n", $1, $4, $9, $6, $3; w += $4; c += $9 } END { printf "%-44s %7d %7.1f\n", "total", w, c }'
	exit
fi
# ---------- scripts/areas.tsv: the files a change touched -> the jobs it can break (the CI workflow and `affected` share it)
source "$REPO/scripts/areas.sh"
areas_load "$REPO/scripts/areas.tsv"
# Static map API, also used by parity fixtures; never starts a suite.
if [[ $MODE == --areas-regex ]]; then
	areas_load "$2"
	areas_regex "${@:3}"
	exit
fi
# --github prints the outputs of the workflow: jobs (the regex), the flags of its other jobs, nshards (a long queue is cut in two), full (it is all of it)
if [[ $MODE == affected ]]; then
	shift
	OUT=pick
	[[ ${1:-} == --list || ${1:-} == --github ]] && { OUT=${1#--}; shift; }
	BASE=${1:-origin/main}
	if [[ $BASE == all ]]; then RE=. FASTRE=.
	elif [[ $BASE == --files ]]; then   # for tests of the map: the jobs of named files
		RE=$(areas_regex "${@:2}") || exit $?
		FASTRE=$(areas_fast_regex "${@:2}") || exit $?
	else
		MB=$(git -C "$REPO" merge-base "$BASE" HEAD) || exit 1
		mapfile -t FILES < <(cd "$REPO" && { git diff --name-only --no-renames "$MB"; git ls-files --others --exclude-standard; } | sort -u)
		RE=$(areas_regex "${FILES[@]}") || exit $?
		FASTRE=$(areas_fast_regex "${FILES[@]}") || exit $?
	fi
	mapfile -t JOBS_ALL < <(env -u SIFT_CI_JOBS -u SIFT_CI_AFFECTED -u SIFT_CI_SHARD SIFT_CI_PG_MAJOR=18 "$0" --plan quick | cut -d' ' -f1)
	PICK=()
	for f in "$REPO"/test/sql/*.sql; do [[ installcheck-$(basename "$f" .sql) =~ $RE ]] && BATCHES=1; done
	for id in "${JOBS_ALL[@]}"; do [[ $id == installcheck-batch* && -n ${BATCHES:-} || $id =~ $RE ]] && PICK+=("$id"); done
	flag() { [[ gh:$1 =~ $RE ]] && echo true || echo false; }
	# the fast tier of a pull request (SIFT_CI_JOBS): the regression tests, the cheap checks, and the tests the diff itself touches; the rest runs on main
	FAST='^(installcheck-|isolation-emulated$|coverage$)'
	for f in ${FILES[@]+"${FILES[@]}"}; do
		if [[ $f =~ ^test/t/(.+)\.(pl|sh)$ ]]; then FAST+="|^tap(-shell)?-${BASH_REMATCH[1]}\$"
		elif [[ $f == test/isolation/* ]]; then FAST+='|^isolation$'
		elif [[ $f == test/col/* ]]; then FAST+='|^difftest-col|^plan-gate$'
		elif [[ $f == test/fuzz/diff_fuzz* ]]; then FAST+='|^difftest-fuzz$'
		elif [[ $f == test/plan/* || $f == scripts/plangate.sh ]]; then FAST+='|^plan-gate$'; fi
	done
	case $OUT in
		list) echo "${#PICK[@]} of ${#JOBS_ALL[@]} jobs: ${PICK[*]}"; for f in sqlite mariadb replication macos; do echo "gh:$f $(flag $f)"; done ;;
		github)
			echo "jobs=$RE"
			echo "fast=$FAST"
			echo "fastjobs=$FASTRE"
			echo "pg=$( (( ${#PICK[@]} )) && echo true || echo false)"
			for f in sqlite mariadb replication macos; do echo "$f=$(flag $f)"; done
			echo "nshards=$( (( ${#PICK[@]} >= ${SIFT_CI_SHARD_MIN:-40} )) && echo 2 || echo 1)"
			echo "full=$( (( ${#PICK[@]} == ${#JOBS_ALL[@]} )) && [[ $(flag sqlite)$(flag mariadb)$(flag replication) == truetruetrue ]] && echo true || echo false)" ;;
		pick) "$REPO/scripts/check-neutral.sh" || exit 1   # a change of any file: the wording guard is not a job of the map
			(( ${#PICK[@]} )) || { echo "nothing to run: the diff touches no job"; exit 0; }
			echo "ci: ${#PICK[@]} of ${#JOBS_ALL[@]} jobs" >&2
			SIFT_CI_AFFECTED=$RE exec "$0" quick ;;
	esac
	exit 0
fi
COL_SLICES_LEAN=4 COL_SLICES_DEEP=8   # slices of the column differential jobs of the full tier (below): the deep tier cuts them finer so that no job passes the hour
COL_SLICES=$COL_SLICES_LEAN; [[ -z ${SIFT_CI_DEEP:-} ]] || COL_SLICES=$COL_SLICES_DEEP
declare -A JOB_WEIGHT=(
	[difftest]=1398 [difftest-col-3]=1067 [difftest-mutants]=1017 [difftest-col-tbl-1]=914 [difftest-col-1]=882 [difftest-col-tbl-4]=877
	[difftest-col-4]=849 [difftest-col-tbl-2]=800 [difftest-col-2]=789 [difftest-col-tbl-3]=675 [tap-shell-compat_matrix]=578 [fuzz]=558
	[tap-001_crash]=504 [installcheck-col_key_presence]=450 [tap-shell-store_crash]=341 [tap-shell-col_index_crash]=283
	[installcheck-col_topn_parallel]=252 [installcheck-agg_group_spill_fuzz]=248 [installcheck-tbl_writepath]=246 [difftest-nopos]=236 [difftest-fuzz]=204
	[tap-shell-index_build_crash]=197 [soak]=195 [tap-shell-col_auto_merge_partition]=194 [installcheck-col_sorted_run]=186 [tap-014_tbl_writepath]=181
	[tap-006_truncate]=178 [tap-shell-index_random_crash]=165 [installcheck-agg_preagg_rollup]=158 [installcheck-tbl_preagg_calendar]=154 [hardened]=152
	[coverage-pct]=126 [installcheck-col_scan]=124 [soak-col]=123 [tap-003_wal_minimal]=117 [installcheck-tbl_preagg]=115 [isolation]=113
	[tap-shell-maint_fanin]=112 [tap-shell-col_crash_matrix]=111 [installcheck-agg_set_array]=111 [installcheck-build_large_docs]=107
	[tap-017_tbl_autodefrag]=102 [installcheck-agg_preagg_filter]=102 [tap-shell-col_wbuf_crash]=101 [tap-shell-compat_31_column]=99
	[installcheck-agg_tid_range]=98 [installcheck-facets]=96 [isolation-emulated]=94 [installcheck-parallel_scan]=91
)
JOB_WEIGHT_DEFAULT=10
# the jobs whose size the deep tier changes: lane seconds of the deep run 40079220375 on PostgreSQL 18 (the slices are the eighth of difftest-col 5860 s and difftest-col-tbl 5101 s)
declare -A JOB_WEIGHT_DEEP=([difftest]=1949 [difftest-mutants]=1281 [fuzz]=728 [soak]=613 [difftest-fuzz]=510 [difftest-nopos]=474 [soak-col]=303 [soak-gin]=136 [difftest-col-analytics]=150
	[difftest-col-1]=714 [difftest-col-2]=714 [difftest-col-3]=714 [difftest-col-4]=714 [difftest-col-5]=714 [difftest-col-6]=714 [difftest-col-7]=714 [difftest-col-8]=714
	[difftest-col-tbl-1]=638 [difftest-col-tbl-2]=638 [difftest-col-tbl-3]=638 [difftest-col-tbl-4]=638 [difftest-col-tbl-5]=638 [difftest-col-tbl-6]=638 [difftest-col-tbl-7]=638 [difftest-col-tbl-8]=638)
# weight_of JOB [deep]: the weight in seconds of a lane
weight_of() { if [[ -n ${2:-} && -v JOB_WEIGHT_DEEP[$1] ]]; then echo "${JOB_WEIGHT_DEEP[$1]}"; else echo "${JOB_WEIGHT[$1]:-$JOB_WEIGHT_DEFAULT}"; fi; }
# --plan [quick|full]: the job plan (job, suite, pool or after, command) without running anything; --selftest: checks the plan of every
# PostgreSQL major (SIFT_CI_PG_MAJOR overrides the server's) against the rule that no job installs a library flavor while the lanes run
if [[ $MODE == --selftest ]]; then
	bad=0
	for pg in 16 17 18; do
		for m in quick full; do
			plan=$(env -u SIFT_CI_JOBS -u SIFT_CI_AFFECTED -u SIFT_CI_SHARD SIFT_CI_PG_MAJOR=$pg "$0" --plan $m) || { echo "FAIL: --plan $m on PostgreSQL $pg"; bad=1; continue; }
			n=0
			while read -r id suite where cmd; do
				# a job whose script runs `pgtest.sh install` builds a flavor of its own: before 18 that is the server's library
				for w in $cmd; do [[ $w == */scripts/*.sh && -f $w ]] && { grep -E 'pgtest\.sh"? install' "$w" | grep -qvE '^\s*#|echo'; } && break || w=; done
				[[ -n $w ]] || continue
				n=$((n + 1))
				want=pool; (( pg < 18 )) && want=after
				[[ $where == "$want" ]] || { echo "FAIL: PostgreSQL $pg $m: $id installs a flavor and is placed $where, want $want"; bad=1; }
			done <<<"$plan"
			(( n )) || { echo "FAIL: PostgreSQL $pg $m: the plan has no job that installs a flavor (the check is void)"; bad=1; }
		done
	done
	# scripts/areas.tsv: every tracked file is in a row, and every job of the plans (batches of short tests are cut from the tests of the areas) is in a group, so that
	# neither a file nor a test is left out of the selection; a core file selects everything
	mapfile -t tracked < <(git -C "$REPO" ls-files)
	miss=$(areas_regex "${tracked[@]}" 2>&1 >/dev/null) && [[ -z $miss ]] || { echo "FAIL: $miss"; bad=1; }
	grouped=$(printf '(%s)|' "${GROUP[@]}")
	for m in quick full; do
		while read -r id _; do
			[[ $id == installcheck-batch* || $id =~ ${grouped%|} ]] || { echo "FAIL: areas: the $m job $id is in no group of scripts/areas.tsv"; bad=1; }
		done < <(env -u SIFT_CI_JOBS -u SIFT_CI_AFFECTED -u SIFT_CI_SHARD SIFT_CI_PG_MAJOR=18 "$0" --plan $m)
	done
	for f in "$REPO"/test/sql/*.sql; do
		[[ installcheck-$(basename "$f" .sql) =~ ${grouped%|} ]] || { echo "FAIL: areas: the test $(basename "$f") is in no group of scripts/areas.tsv"; bad=1; }
	done
	# the fast tier: a source file runs the tests of its own area (src/agg: not the ones only the table area owns), a core file all of them
	fast_agg=$(areas_fast_regex src/agg/x.c) fast_core=$(areas_fast_regex src/core/ops.c)
	while read -r id _; do
		[[ $id == installcheck-* ]] || continue
		[[ $id =~ $fast_core ]] || { echo "FAIL: areas: src/core/ops.c does not select the fast job $id"; bad=1; }
		[[ $id =~ ${GROUP[tbl]} && ! $id =~ ${GROUP[agg]} && ! $id =~ ${GROUP[col]} && $id =~ $fast_agg ]] && { echo "FAIL: areas: a src/agg file selects the table-only fast job $id"; bad=1; }
		[[ $id =~ ${GROUP[agg]} && ! $id =~ $fast_agg ]] && { echo "FAIL: areas: a src/agg file does not select the agg fast job $id"; bad=1; }
	done < <(env -u SIFT_CI_JOBS -u SIFT_CI_AFFECTED -u SIFT_CI_SHARD SIFT_CI_PG_MAJOR=18 "$0" --plan quick)
	for f in src/core/ops.c "$REPO"/sql/*.sql; do
		f=${f#"$REPO"/}
		[[ $(areas_regex "$f") == *'|.' ]] || { echo "FAIL: areas: $f does not select every job"; bad=1; }
	done
	for deep in '' 1; do
		cs=$COL_SLICES_LEAN; [[ -z $deep ]] || cs=$COL_SLICES_DEEP
		# the column differential jobs of the full tier are slices of one corpus (lean 4, deep 8): K/N for K = 1..N, nothing else different between them (no subset of the variants,
		# scenarios or queries), each slice has queries, no query is in two, and together they are the corpus the unsliced script selects
		plan=$(env -u SIFT_CI_JOBS -u SIFT_CI_AFFECTED -u SIFT_CI_SHARD SIFT_CI_DEEP=$deep SIFT_CI_PG_MAJOR=18 "$0" --plan full)
		for prefix in difftest-col difftest-col-tbl; do
			n=0 ks='' rest='' union='' cd=''
			while read -r id _ _ cmd; do
				[[ $id =~ ^$prefix-[0-9]+$ ]] || continue
				eval "args=($cmd)"
				cd=${args[0]}; k=''; mine=()
				for (( i = 1; i < ${#args[@]}; i++ )); do
					if [[ ${args[i]} == --slice ]]; then k=${args[++i]}; else mine+=("${args[i]}"); fi
				done
				[[ $k =~ ^([0-9]+)/([0-9]+)$ ]] || { echo "FAIL: $id has no --slice K/N"; bad=1; continue; }
				[[ " ${mine[*]} " != *" --variants "* && " ${mine[*]} " != *" --scenarios "* && " ${mine[*]} " != *" --corpus "* ]] ||
					{ echo "FAIL: $id runs a subset of the variants, scenarios or queries (--scenario-slices alone may differ from the deep run)"; bad=1; }
				[[ -z $rest || $rest == "${mine[*]}" ]] || { echo "FAIL: the slices of $prefix differ in more than --slice ($id)"; bad=1; }
				rest=${mine[*]} n=$((n + 1)) ks+=" ${BASH_REMATCH[1]}"
				[[ ${BASH_REMATCH[2]} == "$cs" ]] || { echo "FAIL: $id is a slice of ${BASH_REMATCH[2]}, COL_SLICES is $cs"; bad=1; }
				part=$("$cd" --list "${mine[@]}" --slice "$k") || { echo "FAIL: $id: coldiff.sh --list"; bad=1; continue; }
				[[ -n $part ]] || { echo "FAIL: $id selects no query"; bad=1; }
				union+=$part$'\n'
			done <<<"$plan"
			(( n == cs )) || { echo "FAIL: $n slices of $prefix in the full plan, want $cs"; bad=1; }
			[[ $(tr ' ' '\n' <<<"$ks" | grep . | sort -n | tr '\n' ' ') == "$(seq -s ' ' 1 "$cs") " ]] || { echo "FAIL: the slices of $prefix are not 1..$cs:$ks"; bad=1; }
			if [[ -n $cd ]]; then
				whole=$(cd "$REPO/test/col/fixtures" && printf '%s\n' *.sql)   # the directory, not coldiff.sh's own idea of it
				[[ -z $(sort <<<"$union" | grep . | uniq -d) ]] || { echo "FAIL: a query is in two slices of $prefix: $(sort <<<"$union" | grep . | uniq -d | head -3 | tr '\n' ' ')"; bad=1; }
				[[ $(sort <<<"$union" | grep .) == "$(sort <<<"$whole")" ]] || { echo "FAIL: the slices of $prefix are not the whole corpus ($(grep -c . <<<"$union") of $(grep -c . <<<"$whole") queries)"; bad=1; }
			fi
		done
		grep -q '^difftest-col-analytics ' <<<"$plan" || { echo "FAIL: the full plan ${deep:+(deep) }lacks difftest-col-analytics (the analytics suite is not in the slices)"; bad=1; }
	done
	# the lean full tier before PostgreSQL 18 runs half of the slices (the first and second of 4), the deep one all of them
	for pg in 16 17; do
		for deep in '' 1; do
			n=$(env -u SIFT_CI_JOBS -u SIFT_CI_AFFECTED -u SIFT_CI_SHARD SIFT_CI_DEEP=$deep SIFT_CI_PG_MAJOR=$pg "$0" --plan full | grep -cE '^difftest-col-[0-9]+ ')
			(( n == (deep ? COL_SLICES_DEEP : COL_SLICES_LEAN / 2) )) || { echo "FAIL: PostgreSQL $pg ${deep:+deep }full plan has $n slices of difftest-col"; bad=1; }
		done
	done
	# the shards of the full tier: every job once, and the load by the weights within 15% of the least loaded shard
	for pg in 17 18; do
	for deep in '' 1; do
		ns=3; (( pg == 17 )) && ns=5
		[[ -z $deep ]] || { ns=4; (( pg == 17 )) && ns=9; }
		all=$(env -u SIFT_CI_SHARD SIFT_CI_DEEP=$deep SIFT_CI_PG_MAJOR=$pg "$0" --plan full | awk '{print $1}' | sort)
		got=$(for (( k = 1; k <= ns; k++ )); do env SIFT_CI_SHARD=$k/$ns SIFT_CI_DEEP=$deep SIFT_CI_PG_MAJOR=$pg "$0" --plan full | awk '{print $1}'; done | sort)
		[[ $all == "$got" ]] || { echo "FAIL: the $ns shards of the full tier on PostgreSQL $pg ${deep:+deep }are not the plan, each job once"; bad=1; }
		lo='' hi=0
		for (( k = 1; k <= ns - (pg < 18 ? 1 : 0); k++ )); do
			l=0
			while read -r j _; do l=$(( l + $(weight_of "$j" "$deep") )); done < <(env SIFT_CI_SHARD=$k/$ns SIFT_CI_DEEP=$deep SIFT_CI_PG_MAJOR=$pg "$0" --plan full)
			(( l > hi )) && hi=$l
			[[ -z $lo ]] || (( l < lo )) && lo=$l
		done
		(( hi * 100 <= lo * 115 )) || { echo "FAIL: the shards of the full tier on PostgreSQL $pg ${deep:+deep }are unbalanced by the weights of scripts/ci.sh (heaviest $hi s, lightest $lo s)"; bad=1; }
	done
	done
	# a released extension script is never edited (an install upgraded in place would miss the change): new objects go into the next version's scripts
	(cd "$REPO" && scripts/sha256.sh --quiet -c scripts/released-sql.sha256) || { echo "FAIL: a released sql/pg_sift--*.sql differs from its tag; add the change to the scripts of the next version (a release appends its own scripts to scripts/released-sql.sha256)"; bad=1; }
	# scripts/leftovers.allow: a test that exists, an object and a reason on every line
	re='^([a-z0-9_]+): .+ # .+$'
	while IFS= read -r l; do
		[[ -z $l || $l == '#'* ]] && continue
		[[ $l =~ $re && -f $REPO/test/sql/${BASH_REMATCH[1]}.sql ]] || { echo "FAIL: scripts/leftovers.allow: \"$l\" is not \"test: regex # reason\" of an existing test"; bad=1; }
	done < "$REPO/scripts/leftovers.allow"
	"$REPO/scripts/check-neutral.sh" --selftest || bad=1
	"$REPO/scripts/gen-options.sh" --selftest || bad=1
	"$REPO/scripts/check-bench-cases.sh" --selftest || bad=1
	# bash 3.2 (stock macOS) exits 0 on the syntax of bash 4: a script that uses it starts with the check that stops it with a message
	b4='declare -A|local -A|local -n|declare -n|mapfile|readarray|\[\[ -v |\$\{[A-Za-z_]+(,,|\^\^)\}|wait -n|EPOCHSECONDS|EPOCHREALTIME|printf .%\(|&>>|\|&|coproc|;;&|read -[a-zA-Z]*N'
	for f in $(git -C "$REPO" ls-files 'scripts/*.sh' 'test/t/*.sh' 'test/t/*.bash'); do
		! grep -qE "$b4" "$REPO/$f" || grep -q 'needs bash 4+' "$REPO/$f" || { echo "FAIL: $f uses bash 4 and lacks the first-line check of scripts/ci.sh"; bad=1; }
	done
	# the checksum tool of macOS is BSD's (`-c --quiet` needs a file operand) and older releases have none: scripts/sha256.sh is the one caller (the pattern is split so that this line is not a match)
	h='sha256''sum|md5''sum'
	for f in $(git -C "$REPO" ls-files 'scripts/*.sh' 'test/t/*.sh' 'test/t/*.bash'); do
		[[ $f == scripts/sha256.sh ]] || ! grep -qE "\\b($h)\\b" "$REPO/$f" || { echo "FAIL: $f calls the system checksum tool; use scripts/sha256.sh"; bad=1; }
	done
	# the loud failures: bash older than 4 (the check made to fail), and a run that selects no job, stop before anything is built
	t=$(mktemp)
	sed 's/>= 4/>= 99/' "$0" >"$t"
	out=$(env -u SIFT_CI_TMP bash "$t" quick 2>&1) && { echo "FAIL: ci.sh exits 0 on bash older than 4"; bad=1; }
	[[ $out == *'needs bash 4+'* ]] || { echo "FAIL: ci.sh does not say it needs bash 4+: $out"; bad=1; }
	# not in the scratch directory and the shard of the run that calls this: a shard may select no job and still builds, in the directory it removes first and at its end
	out=$(env -u SIFT_CI_TMP -u SIFT_CI_SHARD SIFT_CI_JOBS='^none$' "$0" quick 2>&1) && { echo "FAIL: ci.sh exits 0 when no job is selected"; bad=1; }
	[[ $out == *'no test job selected'* ]] || { echo "FAIL: ci.sh does not say that no job is selected: $out"; bad=1; }
	rm -f "$t"
	(( bad )) || echo "plan-selftest: no flavor installs while the lanes run (PostgreSQL 16, 17, 18; quick, full); every file and job is in scripts/areas.tsv; the column differential slices are the whole corpus"
	exit $bad
fi
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
# run when the directory really exceeds it: twice the budget (full: 8192 up to four lanes, 512 more for each lane above)
if [[ $MODE == full ]]; then
	export SANITIZE=1; BUDGET_MB=${CI_BUDGET_MB:-2000}; MAX_MB=${CI_MAX_MB:-$(( 8192 + 512 * (LANES > 4 ? LANES - 4 : 0) ))}; unset SIFT_QUICK
else
	export SIFT_QUICK=1; BUDGET_MB=${CI_BUDGET_MB:-1200}; MAX_MB=${CI_MAX_MB:-$(( 2 * BUDGET_MB ))}
fi
# CI_JOB_TIMEOUT: seconds a job may run before it is killed (quick 3000, full 7200; the slowest job of a shard takes 2100 and 4700 on a loaded runner), so that a
# stuck test fails with the end of its log instead of holding the runner until the limit of the workflow. Not enforced where there is no timeout(1) (macOS)
JOB_TIMEOUT=()
for tm in timeout gtimeout; do command -v $tm >/dev/null && { JOB_TIMEOUT=($tm -k 20 "${CI_JOB_TIMEOUT:-$([[ $MODE == full ]] && echo 7200 || echo 3000)}"); break; }; done

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
# flavor_job ID SUITE CMD...: a job that installs a library flavor of its own (scripts/pgtest.sh install: hardened, production). On PostgreSQL
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
declare -A EST_MB=([tap-013_tbl_bigtx]=820 [tap-014_tbl_writepath]=950 [installcheck-tbl_writepath]=500 [tap-shell-old_lib_refusal]=150 [tap-shell-scaling_slope]=1400 [installcheck-tbl_bigtx]=700 [tap-004_wal_consistency]=690 [tap-shell-store_replica]=500 [tap-006_truncate]=550 [tap-010_tbl_settings]=420
	[tap-006_tbl]=380 [tap-002_standby]=380 [tap-shell-maint_memory]=330 [tap-shell-extension_lifecycle]=260 [installcheck-batch5]=260 [installcheck-index_build_postings]=230
	[tap-shell-replication]=200 [tap-shell-store_concurrency]=200 [tap-shell-index_build_crash]=180
	[tap-shell-store_crash]=170 [tap-001_crash]=150 [installcheck-index_build_thresholds]=140)
JOB_MB=70

tap_perl() { local f; for f in "$@"; do job "tap-$f" tap 0 "$S/tap.sh" "t/$f.pl"; done; }
tap_shell() { local f; for f in "$@"; do job "tap-shell-$f" tap-shell 0 "$S/pgtest.sh" tap "$f"; done; }
installcheck() { local t; for t in "$@"; do job "installcheck-$t" installcheck 1 "$S/pgtest.sh" regress "$t"; done; }
# installcheck_batches N TEST...: the short tests, N to a pg_regress run in one database like make installcheck runs them: a run costs 0.3 s of
# CPU and lane time before its first statement, which is more than most of these tests need. The tests that VACUUM keep a database of
# their own: VACUUM removes (and a sift_table seals) only the rows older than every snapshot of the database, and an autovacuum worker on
# the tables an earlier test left behind holds one
installcheck_batches() {
	local n=$1 i=0 b=() t
	shift
	for t in "$@"; do
		[[ installcheck-$t =~ ${SIFT_CI_AFFECTED:-.} ]] || continue
		if grep -qiE 'using sift_table|\<vacuum\>' "$REPO/test/sql/$t.sql"; then installcheck "$t"; continue; fi
		b+=("$t")
		(( ${#b[@]} < n )) || { job "installcheck-batch$((++i))" installcheck 1 "$S/pgtest.sh" regress "${b[@]}"; b=(); }
	done
	(( ${#b[@]} == 0 )) || job "installcheck-batch$((++i))" installcheck 1 "$S/pgtest.sh" regress "${b[@]}"
}

# sessions of a differential test: its parallel stages start 3 workers each and a cluster allows 8, the mutants run on the reference only
DIFF_JOBS=$(( LANES > 1 ? 2 : 1 )) MUTANT_JOBS=$(( LANES > 1 ? 4 : 1 ))
# by measured time, the long jobs of each kind first
SLOW_REGRESS="tbl_bigtx tbl_writepath build_large_docs index_build_postings filter_pushdown facets parallel_scan insert_backpressure parallel_pushdown index_build_thresholds
	edge_documents rank_semi_join analyze_invariant rank_join rank_prune phrase_positions kernels_simd col_kernels_simd col_enc parallel_build"
SLOW_PERL="001_crash 004_wal_consistency"
SLOW_SHELL="leakcheck_gate store_crash replication store_replica index_build_crash store_concurrency col_wbuf_crash"
# shellcheck disable=SC2046,SC2086  # the names are words
if [[ $MODE == full ]]; then
	# The full tier has two sizes. The lean one (every dispatch and the run that is the evidence of a release) fits a job in 45 minutes and the run in 75 on the hosted
	# runners: the same jobs, the same checks, shorter loops, a smaller corpus, fixed seeds. The deep one (SIFT_CI_DEEP=1: the nightly run, or a dispatch with deep) has the
	# sizes of before: lean_or_deep LEAN DEEP
	lean_or_deep() { [[ -n ${SIFT_CI_DEEP:-} ]] && echo "$2" || echo "$1"; }
	job soak soak 1 "$S/soak.sh" --duration "$(lean_or_deep 180 600)" --check-every 30
	job soak-gin soak-gin 1 "$S/soak.sh" --am gin --duration "$(lean_or_deep 60 120)" --rows 20000 --clients 2 --check-every 20
	job soak-col soak-col 1 "$S/colsoak.sh" --duration "$(lean_or_deep 120 300)"
	job difftest-mutants difftest-mutants 1 "$S/difftest.sh" --jobs "$MUTANT_JOBS" --engine mutants --docs "$(lean_or_deep 4000 8000)" --queries 40 --rounds "$(lean_or_deep 1 2)" --minimize 1
	job difftest difftest 1 "$S/difftest.sh" --jobs "$DIFF_JOBS" --docs "$(lean_or_deep 4000 5000)" --queries "$(lean_or_deep 100 150)" --rounds "$(lean_or_deep 3 4)"
	job difftest-nopos difftest-nopos 1 env "INDEX_OPTS=WITH (positions = off)" "$S/difftest.sh" --jobs "$DIFF_JOBS" --docs "$(lean_or_deep 2500 3000)" --queries "$(lean_or_deep 40 60)" --rounds "$(lean_or_deep 1 2)"
	# the shape tags and 0 DIFFER hold the same at 1500 fixture rows and 20000 analytics rows as at 4000 and 100000 (395 s against 483 s)
	# the two differential tests of the column index take 160 minutes of one lane each on PostgreSQL 18, and more on 17, longer than the rest of the tier together
	# in four lanes: they are cut into COL_SLICES disjoint slices of the corpus (coldiff.sh --slice K/N) that the lanes and the shards take like any other job. Every
	# slice runs every variant and scenario on its share of the queries (--selftest proves the slices are the whole corpus, each query once)
	# Lean: each scenario runs a third of the scenario queries (the slice moves from one heap state to the next, so every query meets several states and every state
	# meets queries of every kind), and before PostgreSQL 18 half the corpus (slices 1 and 2 of 4, every other query in name order: the corpus is the same on every major,
	# the other half runs on 18)
	for (( k = 1; k <= COL_SLICES; k++ )); do
		(( PG_MAJOR >= 18 || k <= COL_SLICES / 2 )) || [[ -n ${SIFT_CI_DEEP:-} ]] || continue
		job difftest-col-$k difftest-col 1 "$S/coldiff.sh" --suite fixtures --slice $k/$COL_SLICES --fixture-rows 1500 --scenario-slices "$(lean_or_deep 3 1)"
		job difftest-col-tbl-$k difftest-col-tbl 1 "$S/coldiff.sh" --table --suite fixtures --slice $k/$COL_SLICES --fixture-rows 1500 --scenario-slices "$(lean_or_deep 3 1)"
	done
	job difftest-col-analytics difftest-col 1 "$S/coldiff.sh" --suite analytics --rows 20000
	job difftest-col-selftest difftest-col-selftest 1 "$S/coldiff.sh" --selftest
	# the plan the planner picks, the cost ratios and the results of 96 shapes at 50000 rows; a timing that loses only warns (hosted runners are noisy)
	job plan-gate plan-gate 1 "$S/plangate.sh" --rows 50000 --runs 3 --warn-time
	# seeded random queries of the pushdown shapes, plain plan against the scan, the pre-aggregates and the arms alone (test/fuzz/diff_fuzz.py); --leader-on until the
	# parallel scan of a partitioned table without leader participation no longer crashes a worker
	job difftest-fuzz difftest-fuzz 1 python3 "$REPO/test/fuzz/diff_fuzz.py" --seeds "$(lean_or_deep 1-6 1-12)" --queries "$(lean_or_deep 100 150)" --rows 6000 --leader-on
	job coverage coverage 1 "$S/coverage.sh" --check
	job reference-live reference-live 1 bash -c 'eval "$("$0/scripts/pgtest.sh" env)"; exec "$0/scripts/gen-reference.sh" --live' "$REPO"
	job options-live options-live 1 bash -c 'eval "$("$0/scripts/pgtest.sh" env)"; exec "$0/scripts/gen-options.sh" --live' "$REPO"
	# the share of the variant matrices the pushdown answers (docs/results/coverage-pct.tsv): the full tier recomputes the committed counts, the quick tier is a smoke of a third of the variants
	job coverage-pct coverage-pct 1 "$S/coverage.sh" --pct --verify
	job fuzz fuzz 1 "$S/fuzz.sh" --scale "$(lean_or_deep 4 10)"
	flavor_job hardened hardened "$S/hardened.sh"
	job static static 0 "$S/static.sh"
	tap_perl $(named $SLOW_PERL) $(rest t .pl $SLOW_PERL)
	tap_shell $(named $SLOW_SHELL) $(rest t .sh $SLOW_SHELL)
	installcheck $(named $SLOW_REGRESS)
	installcheck_batches 12 $(rest sql .sql $SLOW_REGRESS)
	job isolation isolation 1 "$S/isolation.sh"
	job isolation-emulated isolation-emulated 1 "$S/isolation.sh" --emulate
	job isa-baseline isa-baseline 0 "$S/isa_check.sh"
else
	# 2500 documents: past 2200 the corpus crosses a superblock boundary (the drop_sb_boundary mutant survives below it); 12 queries and 2 rounds still
	# visit every stage, the write buffers, the promotion and the merge
	job difftest difftest 1 "$S/difftest.sh" --jobs "$DIFF_JOBS" --docs 2500 --queries 12 --rounds 2 --minimize 1 --min-trials 10
	# the regression tests that need no test-only function, against the production library
	# (the quick tests only: the slow ones and the ones that need the test-only build, TESTING_REGRESS of the Makefile, are not run)
	production_tests() {
		local t p
		for t in $(rest sql .sql $SLOW_REGRESS); do
			for p in $(sed -En 's/^TESTING_REGRESS \+?= //p' "$REPO/Makefile"); do [[ $t == ${p//%/*} ]] && continue 2; done
			echo "$t"
		done
	}
	flavor_job production-regress production-regress "$S/production-check.sh" $(production_tests)
	tap_perl 004_wal_consistency   # the biggest in scratch: it starts while the budget is free
	installcheck facets index_build_postings
	# one pass (--budget 1) of every target: its seeds and the inputs of the fixed findings are the same, the random and exhaustive loops shorter
	job fuzz fuzz 1 env FZ_SECS=2 FZ_KERNELS_MILLIONS=0.05 FZ_GRID_STRIDE=7 FZ_CODEC_ITERS=100 FZ_ANALYZE_ITERS=5000 "$S/fuzz.sh" --budget 1 ${SIFT_FUZZ_TARGETS:-}
	flavor_job hardened hardened "$S/hardened.sh" colcodec_basic col_enc kernels_simd shred_values verify_corrupt query_errors
	job difftest-nopos difftest-nopos 1 env "INDEX_OPTS=WITH (positions = off)" "$S/difftest.sh" --jobs "$DIFF_JOBS" \
		--docs 1000 --queries 12 --rounds 2 --minimize 1 --min-trials 10
	# the mutants are checked on the built corpus (round 0): all 8 die there, and the rounds with DML, promotion, merge and REINDEX without a bug are the job difftest
	job difftest-mutants difftest-mutants 1 "$S/difftest.sh" --jobs "$MUTANT_JOBS" --engine mutants --docs 3000 --queries 10 --rounds 0 --minimize 0 \
		--mutants "float8_score idf_df_plus1 count_plus drop_high_offset drop_sb_boundary tie_ctid_desc nofold_query topk_short"
	job difftest-col difftest-col 1 "$S/coldiff.sh" --fixture-rows 1500 --rows 3000 --variants on,on-par --scenarios dml,abort,vacuum,held-lite --scenario-slices 3
	job difftest-col-tbl difftest-col-tbl 1 "$S/coldiff.sh" --table --suite fixtures --fixture-rows 1500 --variants on,on-par --scenarios dml,abort,vacuum --scenario-slices 3
	job difftest-col-selftest difftest-col-selftest 1 "$S/coldiff.sh" --selftest --fixture-rows 1200
	job plan-gate plan-gate 1 "$S/plangate.sh" --runs 0
	job difftest-fuzz difftest-fuzz 1 python3 "$REPO/test/fuzz/diff_fuzz.py" --seeds 1-4 --queries 100 --rows 3000 --leader-on
	job coverage coverage 1 "$S/coverage.sh" --check
	job reference-live reference-live 1 bash -c 'eval "$("$0/scripts/pgtest.sh" env)"; exec "$0/scripts/gen-reference.sh" --live' "$REPO"
	job options-live options-live 1 bash -c 'eval "$("$0/scripts/pgtest.sh" env)"; exec "$0/scripts/gen-options.sh" --live' "$REPO"
	job coverage-pct coverage-pct 1 "$S/coverage.sh" --pct --tier quick
	tap_shell leakcheck_gate
	tap_perl 001_crash
	installcheck filter_pushdown
	job soak soak 1 "$S/soak.sh" --duration 15 --rows 20000 --rate 400 --clients 2 --check-every 5
	job soak-col soak-col 1 "$S/colsoak.sh" --duration 18 --clients 6
	tap_perl $(named $SLOW_PERL)
	tap_shell $(named $SLOW_SHELL)
	installcheck $(named $SLOW_REGRESS)
	tap_perl $(rest t .pl $SLOW_PERL)
	tap_shell $(rest t .sh $SLOW_SHELL)
	installcheck_batches 12 $(rest sql .sql $SLOW_REGRESS)
	job isolation isolation 1 "$S/isolation.sh"
	job isolation-emulated isolation-emulated 1 "$S/isolation.sh" --emulate cic_insert
	job isa-baseline isa-baseline 0 "$S/isa_check.sh"
fi
# opt-in lanes on a PostgreSQL built from source (scripts/pg-assert.sh, cached outside the repo; docs/SAFETY.md): queued only when
# SIFT_CI_JOBS names them, e.g. `SIFT_CI_JOBS=assert scripts/ci.sh quick`
[[ ${SIFT_CI_JOBS:-} != *assert* ]] || job assert assert 0 "$S/assert.sh"
[[ ${SIFT_CI_JOBS:-} != *valgrind* ]] || job valgrind valgrind 0 "$S/valgrind.sh"
# SIFT_CI_SHARD=K/N: every Nth job of the queue from the Kth, so that N runners share the pool and each keeps the longest-first order; shard 1 runs
# the checks of the harness. Before PostgreSQL 18 the jobs that run after the pool (flavor installs, serial) are all the last shard's
# The full tier cuts the queue by weight instead (JOB_WEIGHT: seconds of a lane, the jobs of 90 s and more measured on PostgreSQL 18 in the lean run 40097165396 of
# 2026-10-24, 13:12 UTC; the others weigh 10): the heaviest job goes to the shard with the least load so far, which put the three shards of PostgreSQL 18 at 98, 118 and
# 164 lane-minutes by position and balances them to within a few percent (--selftest checks it, and that the shards are the plan exactly once). The deep sizes are
# about 1.5 to 2.5 times the lean ones on the same jobs, the same order.
# shard_loads N POOL: SHARD_OF[job] (0-based shard of the pool) and LOAD[k] for the jobs of the pool, heaviest first, ties in queue order
declare -A SHARD_OF
shard_loads() {
	local pool=$1 line id k i
	SHARD_OF=() LOAD=()
	for (( k = 0; k < pool; k++ )); do LOAD[k]=0; done
	while read -r line; do
		id=${line#* }; k=0
		for (( i = 1; i < pool; i++ )); do (( LOAD[i] < LOAD[k] )) && k=$i; done
		SHARD_OF[$id]=$k LOAD[k]=$(( LOAD[k] + ${line% *} ))
	done < <(for id in "${J_ID[@]}"; do [[ -v J_AFTER[$id] ]] || echo "$(weight_of "$id" "${SIFT_CI_DEEP:-}") $id"; done | sort -s -k1,1nr)
}
SHARD=1
if [[ ${SIFT_CI_SHARD:-} =~ ^([0-9]+)/([0-9]+)$ ]]; then
	SHARD=${BASH_REMATCH[1]} NSHARDS=${BASH_REMATCH[2]} KEEP_IDS=() SUITES=() n=0
	POOL=$(( NSHARDS - (PG_MAJOR < 18 && NSHARDS > 1 ? 1 : 0) ))
	[[ $MODE == full && $NSHARDS -gt 1 ]] && shard_loads "$POOL"
	for id in "${J_ID[@]}"; do
		if [[ -v J_AFTER[$id] ]]; then (( NSHARDS > 1 && SHARD == NSHARDS )) || (( NSHARDS == 1 )) || continue
		elif [[ $MODE == full && $NSHARDS -gt 1 ]]; then (( SHARD_OF[$id] == SHARD - 1 )) || continue
		else (( n++ % POOL == SHARD - 1 )) || continue; fi
		KEEP_IDS+=("$id")
		[[ " ${SUITES[*]-} " == *" ${J_SUITE[$id]} "* ]] || SUITES+=("${J_SUITE[$id]}")
	done
	J_ID=("${KEEP_IDS[@]}")
fi
(( LANES <= ${#J_ID[@]} )) || LANES=$(( ${#J_ID[@]} > 0 ? ${#J_ID[@]} : 1 ))   # a shard with no job still builds
if [[ -n $PLAN ]]; then
	for id in "${J_ID[@]}"; do echo "$id ${J_SUITE[$id]} $([[ -v J_AFTER[$id] ]] && echo after || echo pool) ${J_CMD[$id]}"; done
	exit 0
fi
# a run that selects no job would build, run the harness and exit 0 with no test run (a shard may be empty and still builds)
(( ${#J_ID[@]} || ${NSHARDS:-1} > 1 )) || { echo "ci: no test job selected (SIFT_CI_JOBS='${SIFT_CI_JOBS:-}' SIFT_CI_AFFECTED='${SIFT_CI_AFFECTED:-}')" >&2; exit 1; }

# tidy ID: the data of a job that passed goes at once, so that the scratch directory holds only what runs: its databases in the
# lane's cluster, or its directory after the leak gate scanned it (a finding keeps the directory for the scan of the whole run)
tidy() {
	if [[ ${J_CLUSTER[$1]} == 1 ]]; then "$S/pgtest.sh" clean >/dev/null 2>&1
	else "$S/leakcheck.sh" scan "$SIFT_TMP/$1" >/dev/null 2>&1 && rm -rf "${SIFT_TMP:?}/$1"; fi
}

# claim: sets CLAIMED to the first job of the queue that nobody has claimed and whose estimate fits the budget with the ones
# running (the first job always fits); returns 1 when the queue is empty, 2 when nothing fits yet. The lock is a directory: macOS has no flock
claim() {
	local rc
	until mkdir "$BASE/claim.lock" 2>/dev/null; do
		[[ -d $BASE ]] || { echo "ci: $BASE is gone, the lane stops" >&2; exit 1; }
		sleep 0.02
	done
	claim_locked && rc=0 || rc=$?
	rmdir "$BASE/claim.lock"
	return $rc
}
claim_locked() {
	local id est used=0 left=0 f
	for f in "$BASE"/running/*; do [[ ! -e $f ]] || used=$((used + $(<"$f"))); done
	for id in "${J_ID[@]}"; do
		[[ ! -d $BASE/claims/$id && ! -v J_AFTER[$id] ]] || continue
		left=1
		est=${EST_MB[$id]:-$JOB_MB}
		(( used == 0 || used + est <= BUDGET_MB )) || continue
		mkdir "$BASE/claims/$id"
		echo "$est" >"$BASE/running/$id"
		echo "$LANE" >"$BASE/claims/$id/lane"
		echo "$SECONDS" >"$BASE/claims/$id/start"
		CLAIMED=$id
		return 0
	done
	return $(( left ? 2 : 1 ))
}

# run_job ID: the job with its output in logs/ID.log (a job without the lane's cluster gets a directory of its own)
run_job() {
	step "$1" "${J_SUITE[$1]}" bash -c '[[ $1 == 1 ]] || { SIFT_TMP=$SIFT_TMP/$2; mkdir -p "$SIFT_TMP"; export SIFT_TMP; }; eval "$3"' \
		_ "${J_CLUSTER[$1]}" "$1" "${J_CMD[$1]}"
}

# lane N: takes the jobs the budget admits, in queue order; its cluster starts with the first job that needs one
lane() {
	local id started='' rc
	LANE=lane$1
	export SIFT_TMP=$BASE/$LANE SIFT_PORT=$((BASE_PORT + 10 * ($1 - 1)))
	mkdir -p "$SIFT_TMP"
	while :; do
		claim && rc=0 || rc=$?
		case $rc in 1) break ;; 2) sleep 0.5; continue ;; esac
		id=$CLAIMED
		if [[ ${J_CLUSTER[$id]} == 1 && -z $started ]]; then started=1; "$S/pgtest.sh" start >"$LOG/$LANE.start.log" 2>&1; fi
		run_job "$id" && tidy "$id"
		rm -f "$BASE/running/$id"
	done
	[[ -z $started ]] || "$S/pgtest.sh" stop
}

# not /dev/shm by choice: the run needs about a gigabyte and fsync is off, so RAM buys nothing
# a Unix-domain socket path holds 103 bytes on macOS, where $TMPDIR is a long per-user directory: the clusters' sockets would not fit under it
TMPBASE=${TMPDIR:-/tmp}; [[ $(uname -s) != Darwin ]] || TMPBASE=/tmp
BASE=${SIFT_CI_TMP:-$TMPBASE/sift-ci-$$}
LOG=$BASE/logs
BASE_PORT=${SIFT_PORT:-56130}
# one initdb per run: every cluster starts as a copy (the shell tests and lane clusters, with 1 MB WAL segments; the
# PostgreSQL::Test::Cluster nodes, which are all WAL tests)
export SIFT_ROOT=$BASE/root SIFT_INITDB_TEMPLATE=$BASE/initdb-shell INITDB_TEMPLATE=$BASE/initdb-perl PG_TEST_PORT_DIR=$BASE/ports
rm -rf "$BASE" && mkdir -p "$LOG" "$BASE/claims" "$BASE/running" "$PG_TEST_PORT_DIR"
t0=$SECONDS

watchdog() { # stops every postmaster under $BASE once the scratch directory passes the cap
	local peak=0 mb
	while sleep 2; do
		mb=$(( $(du -sk "$BASE" 2>/dev/null | cut -f1) / 1024 ))
		(( mb > peak )) && { peak=$mb; echo "$peak" >"$LOG/peak_mb"; }
		if (( mb > MAX_MB )); then
			echo "disk cap exceeded: $mb MB > $MAX_MB MB (CI_MAX_MB)" >"$LOG/cap_exceeded"
			find "$BASE" -name postmaster.pid -exec head -1 {} \; | xargs -r kill -QUIT 2>/dev/null
			return
		fi
	done
}
watchdog &
WATCHDOG=$!
# a job that runs long (difftest-col takes two to three hours in the full tier) is silent in the live log until it ends: every 5 minutes the jobs past
# 10 minutes are listed with the last line of their own log, so that a long run is told from a hang by whether that line moves
heartbeat() {
	local f id s
	while sleep 300; do
		for f in "$BASE"/running/*; do
			[[ -e $f ]] || continue
			id=${f##*/}
			[[ -f $BASE/claims/$id/start ]] || continue
			s=$(<"$BASE/claims/$id/start")
			(( SECONDS - s >= 600 )) && printf 'ci: running %s %s s (%s): %s\n' "$id" "$((SECONDS - s))" "$(<"$BASE/claims/$id/lane")" "$(tail -n 1 "$LOG/$id.log" 2>/dev/null | cut -c1-160)"
		done
	done
}
heartbeat &
HEARTBEAT=$!
# SIFT_CI_STATS=1: the footprint of the run by component, lane and memory (scripts/ci_stats.sh), printed after the summary
[[ -z ${SIFT_CI_STATS:-} ]] || { "$S/ci_stats.sh" "$BASE" >"$LOG/stats.txt" 2>&1 & STATS=$!; }
cleanup() {
	kill "$WATCHDOG" "$HEARTBEAT" ${STATS:-} 2>/dev/null
	if [[ -z ${KEEP:-} && ${failed:-1} == 0 ]]; then rm -rf "$BASE"; else echo "scratch kept: $BASE"; fi
}
trap cleanup EXIT

# lane_cpu ID: the CPU seconds the lane cluster's postmaster has spent on the backends and workers it reaped (a job on the lane cluster
# runs its queries there; the other jobs start clusters of their own, which scripts/cpu_time.py counts)
lane_cpu() {
	[[ ${J_CLUSTER[$1]:-0} == 1 && -f $SIFT_TMP/data/postmaster.pid ]] || { echo 0; return; }
	awk '{ print ($16 + $17) / 100 }' "/proc/$(head -1 "$SIFT_TMP/data/postmaster.pid")/stat" 2>/dev/null || echo 0
}
# step ID SUITE CMD...: runs one job or stage with its output in logs/ID.log; appends "id suite status seconds skips lane start end cpu_seconds"
# to the results and returns its status
step() {
	local id=$1 suite=$2 t=$SECONDS status=PASS skips c0 c1 rc=0
	shift 2
	[[ -e $LOG/cap_exceeded ]] && return
	c0=$(lane_cpu "$id")
	${JOB_TIMEOUT[@]+"${JOB_TIMEOUT[@]}"} "$S/cpu_time.py" "$LOG/$id.cpu" "$@" >"$LOG/$id.log" 2>&1 || { rc=$?; status=FAIL; }
	if (( rc == 124 || rc == 137 )); then
		{ echo "ci: $id stopped after $((SECONDS - t)) s (CI_JOB_TIMEOUT), the end of its log:"; tail -n 40 "$LOG/$id.log"; } | tee -a "$LOG/$id.log"
	fi
	c1=$(lane_cpu "$id")
	skips=$(grep -ciE '\bskip' "$LOG/$id.log" || true)
	echo "ci: $status $id $((SECONDS - t)) s ($LANE)"
	printf '%s %s %s %s %s %s %s %s %s\n' "$id" "$suite" "$status" $((SECONDS - t)) "$skips" "$LANE" "$t" "$SECONDS" \
		"$(awk -v a="$(cat "$LOG/$id.cpu" 2>/dev/null || echo 0)" -v c0="$c0" -v c1="$c1" 'BEGIN { printf "%.1f", a + c1 - c0 }')" >>"$LOG/results"
	# the log of a run that is cut (a timeout, a hang) names the jobs that had finished: the results are in the logs artifact of a failed run only
	printf 'ci: %-44s %s %5ss lane %s\n' "$id" "$status" $((SECONDS - t)) "$LANE"
	[[ $status == PASS ]]
}

# ---------- build, the pool, the leak gate ----------
LANE=main
export SIFT_TMP=$BASE/main SIFT_PORT=$BASE_PORT
mkdir -p "$SIFT_TMP"
HARNESS=(leakcheck-selftest plan-selftest portable-core span-test neutral errors reference bench-fresh bench-cases options)
if (( SHARD == 1 )) && [[ -z ${SIFT_CI_NO_HARNESS:-} ]]; then
	step leakcheck-selftest leakcheck-selftest "$S/leakcheck.sh" selftest &
	selftest=$!
	step plan-selftest plan-selftest "$S/ci.sh" --selftest &
	plantest=$!
	step portable-core portable-core "$S/check-portable.sh" &
	portable=$!
	step neutral neutral "$S/check-neutral.sh" &
	neutral=$!
	step errors errors bash -c "\"$S/check-errors.sh\" && \"$S/check-memory.sh\" && \"$S/check-memory.sh\" --selftest" &
	errors=$!
	step reference reference "$S/gen-reference.sh" --check &
	reference=$!
	step bench-fresh bench-fresh "$S/check-bench-fresh.sh" &
	benchfresh=$!
	step bench-cases bench-cases "$S/check-bench-cases.sh" &
	benchcases=$!
	step options options "$S/gen-options.sh" --check &
	options=$!
	step span-test span-test bash -c 'make -s -C "$0/sqlite" B="$SIFT_TMP/span" span-test span-test-asan' "$REPO" &
	spantest=$!
else HARNESS=(); fi
# the second is the node PostgreSQL::Test::Cluster makes, with the environment its test utilities set (untranslated messages)
"$S/pgtest.sh" initdb-template "$SIFT_INITDB_TEMPLATE" || { failed=1; exit 1; }
env -u LANGUAGE -u LC_ALL LC_MESSAGES=C initdb --no-sync -D "$INITDB_TEMPLATE" -A trust >/dev/null || { failed=1; exit 1; }
step build build "$S/pgtest.sh" install || { wait ${selftest:-} ${plantest:-} ${portable:-} ${spantest:-} ${neutral:-} ${errors:-} ${reference:-} ${benchfresh:-} ${benchcases:-} ${options:-}; cat "$LOG/build.log"; failed=1; exit 1; }

pids=()
for (( n = 1; n <= LANES; n++ )); do
	( lane "$n" ) &
	pids+=($!)
done
wait "${pids[@]}" ${selftest:-} ${plantest:-} ${portable:-} ${spantest:-} ${neutral:-} ${errors:-} ${reference:-} ${benchfresh:-} ${benchcases:-} ${options:-}
# the jobs that install a flavor of the server's own library (flavor_job) run alone, now that no worker of a lane is left to load it
for id in "${J_ID[@]}"; do [[ -v J_AFTER[$id] ]] && run_job "$id" && tidy "$id"; done
step leakcheck leakcheck "$S/leakcheck.sh" scan "$BASE"

# ---------- summary ----------
# SUITE: "status seconds skips where" of the jobs of one suite: FAIL when one failed or none ran, the time from the first start
# to the last end, the skip lines summed, the lane (or the number of lanes); then the failed jobs
summarize() {
	local expect=0 id
	for id in "${J_ID[@]}"; do [[ ${J_SUITE[$id]} == "$1" ]] && expect=$((expect + 1)); done
	awk -v s="$1" -v expect="$expect" '$2 == s { n++; sk += $5; if ($3 != "PASS") bad = bad " " $1
			if (!($6 in seen)) { seen[$6] = 1; nl++; lane = $6 }
			if (lo == "" || $7 < lo) lo = $7; if ($8 > hi) hi = $8 }
		END { printf "%s %d %d %s%s\n", (bad != "" || n == 0 || n < expect ? "FAIL" : "PASS"), hi - lo, sk, (nl > 1 ? nl "_lanes" : lane), bad }' "$LOG/results"
}
echo
green=$'\e[32m' red=$'\e[31m' off=$'\e[0m'
[[ -t 1 ]] || green= red= off=
failed=0
# the suites in the order of the report (a suite not listed follows the others)
ORDER=(build "${HARNESS[@]}")
for s in isa-baseline installcheck isolation-emulated isolation fuzz hardened static difftest-mutants soak soak-gin soak-col difftest difftest-nopos tap-shell tap "${SUITES[@]}"; do
	[[ " ${SUITES[*]} " != *" $s "* || " ${ORDER[*]} " == *" $s "* ]] || ORDER+=("$s")
done
for s in "${ORDER[@]}" leakcheck; do
	read -r status secs skips where bad <<<"$(summarize "$s")"
	line=$(printf '%s %-20s %5ss  %s skip lines  (%s)' "$status" "$s" "$secs" "$skips" "${where/_/ }")
	if [[ $status == PASS ]]; then echo "$green$line$off"; else failed=1; echo "$red$line$off"; fi
	# the log of a split suite: the logs of its jobs, in order
	[[ -f $LOG/$s.log ]] || for id in "${J_ID[@]}"; do [[ ${J_SUITE[$id]} == "$s" ]] && { echo "== $id"; cat "$LOG/$id.log"; }; done >"$LOG/$s.log"
	[[ -z ${bad:-} ]] || echo "$red  failed:$bad$off"
	# a batch of regression tests names the tests that failed
	for id in ${bad:-}; do [[ $id != installcheck-batch* ]] || grep -h '^not ok' "$LOG/$id.log" | sed "s/^/$red    /; s/\$/$off/"; done
done
[[ ! -e $LOG/cap_exceeded ]] || { failed=1; echo "$red$(cat "$LOG/cap_exceeded")$off"; }
[[ -z ${STATS:-} ]] || { kill "$STATS"; wait "$STATS"; cat "$LOG/stats.txt"; }
printf 'wall %ss (%s lanes), peak scratch %s MB, logs %s\n' "$((SECONDS - t0))" "$LANES" "$(cat "$LOG/peak_mb" 2>/dev/null || echo '<2')" "$LOG"
exit $failed
