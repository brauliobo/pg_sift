# shellcheck shell=bash
# The files a change touched, mapped to the jobs it can break: `affected`, `--areas-regex`, the focus tokens and the assertions family.
# The CI workflow and `affected` share scripts/areas.tsv through it; it never starts a suite. Sourced by scripts/ci.sh; not a script of its own.
# ---------- scripts/areas.tsv: the files a change touched -> the jobs it can break (the CI workflow and `affected` share it)
source "$REPO/scripts/areas.sh"
source "$REPO/scripts/ci/focus.sh"
# tests DIR SUFFIX: the names of the files test/DIR/**/*SUFFIX (the path under test/DIR without the suffix), in byte order; the jobs, the weights and `affected` name tests so
tests() { (cd "$REPO/test/$1" && find . -name "*$2" | sed "s|^\./||; s|$2\$||" | LC_ALL=C sort); }
areas_load "$REPO/scripts/areas.tsv"
# Static map API, also used by parity fixtures; never starts a suite.
if [[ $MODE == --areas-regex ]]; then
	areas_load "$2"
	areas_regex "${@:3}"
	exit
fi
# --github prints the outputs of the workflow: jobs (the regex), the flags of its other jobs, nshards (a long queue is cut in three), full (it is all of it)
# focus_regex TOKEN...: the jobs of `affected --focus`. A token is FILE or FILE:FUNCTION (FUNCTION * : a change outside any function); the regression tests that
# execute the function (scripts/focus/NAME.tsv, made by scripts/focus-gen.sh), the tests of the area for a function every test executes (the =hot row) and for a
# change outside functions, scripts/areas.tsv as ever for the other files. A function no row has is new or run by no test: its callers are the other tokens.
focus_regex() {
	local t f rest=() star=() sel names extra
	# a file that was split into a folder (scripts/moved-sources.tsv: NEWFOLDER/ OLDFILE FUNCTION-PREFIX): the rows of the table that were made for the old file name its functions
	# still, with the prefix a static got when a second file of the folder began to call it; a token of the new files is also a token of the old one
	extra=$(awk -F'\t' -v toks="$*" '/^#/ { next } { m[++k] = $0 } END { n = split(toks, a, " ")
		for (i = 1; i <= n; i++) { f = a[i]; fn = ""; if (index(f, ":")) { fn = substr(f, index(f, ":") + 1); f = substr(f, 1, index(f, ":") - 1) }
			for (j = 1; j <= k; j++) { split(m[j], r, "\t"); if (index(f, r[1]) == 1) {
				print (fn == "" ? r[2] : r[2] ":" fn); if (fn != "" && r[3] != "" && index(fn, r[3]) == 1) print r[2] ":" substr(fn, length(r[3]) + 1) } } } }' "$REPO/scripts/moved-sources.tsv" 2>/dev/null)
	set -- "$@" $extra
	for t; do
		f=${t%%:*}
		[[ $f =~ ^src/(agg|col|table|bm25)/([^/]+/)?[^/]+\.[ch]$ ]] || { rest+=("$f"); continue; }
		[[ $t != *:'*' ]] || star+=("$f")
	done
	sel=$(focus_select "$@")
	names=$(sed -n 's/^T //p' <<<"$sel" | paste -sd'|' -)
	star+=($(sed -n 's/^H //p' <<<"$sel" | sort -u))
	{ echo '^(coverage|isolation-emulated)$'
	  [[ -z $names ]] || echo "^installcheck-($names)\$"
	  (( ! ${#star[@]} )) || areas_fast_regex "${star[@]}"
	  (( ! ${#rest[@]} )) || areas_regex "${rest[@]}"
	} | paste -sd'|' -
}
# The assertions family of a pull request (the `cassert` output of `affected --github`, run on the PostgreSQL 18 built with --enable-cassert): a change of a source that starts
# workers or handles snapshots or executor hooks (ASSERT_MARK, read from the file: no list to keep) runs the regression tests that run those paths. The `assertions` label
# of a pull request runs the family for any change. A change reached the tip with green fast checks and failed an Assert of the server (GetActiveSnapshot) in a worker.
# The family is one shard on every event: the tests that run workers or take snapshots, and the planner tests (cte, cost, scope, lateral, subplan, placeholder) whose asserts
# (a foreach_node cast of the wrong node type) exist only in a server with assertions. The batches are cut to one for each lane (installcheck_batches), so the shard has no straggler.
ASSERT_MARK='RestoreSnapshot|PushActiveSnapshot|GetActiveSnapshot|ParallelWorkerMain|InitializeParallelDSM|shm_toc_|CreateQueryDesc|ExecutorStart_hook|foreach_node|castNode|create_upper_paths_hook|sift_agg_v6_upper_paths'
ASSERT_JOBS='^(installcheck-(agg/cte/.*|agg/hybrid/.*|agg/par/.*|agg/plan/.*|agg/v6/.*|bm25/parallel/.*|agg/array/image_cost|agg/direct_combo_cost|agg/distinct/cost|agg/distinct/parallel|agg/empty_scope|agg/filter_hashed_subplan_price|agg/flat_rewrites_consolidated|agg/flat_rewrites_consolidated_2|agg/full_join_placeholder|agg/fuse/subplan_key|agg/hash_agg_cost|agg/in_keys_cost|agg/in_list_cost|agg/join/flip_probe_consolidated|agg/jsonb/parallel|agg/jsonb/typeof_parallel|agg/key/entries_cost|agg/lateral_flatten_f2|agg/left_lateral|agg/lookup/read_cost|agg/lookup/shared|agg/narrow_scope|agg/natural_cost|agg/nested_ctes|agg/output_subplan|agg/part/open_cost|agg/part/parity|agg/part/placeholder|agg/preagg/elem_parallel|agg/rollup_cost|agg/rows_parallel|agg/scope/cost|agg/scope/overlay_parity|agg/scoped_lookup_cost|agg/statement_cte_children|agg/strict_placeholders|agg/stub/parallel|agg/subplan_lookup_cost|agg/subplan_lut|agg/text_absent_cost|agg/two_level_cell_qual|agg/two_level_rows_consolidated|agg/two_level_rows_consolidated_2|agg/unkeyed_json_cost|agg/unused_lateral|agg/vet_dirty_cost|agg/wide/numeric_cost|agg/wildcard_key_cost|agg/window/topn_consolidated|agg/zone/cost|col/dirty_recheck_parallel|col/f2/agg_live|col/f2/parallel_conc|col/f2/partitioned_agg_v6|col/f2/scope_live|col/index/parallel|col/proj/analytics_parity|col/scope/parallel|col/topn/parallel|col/topn/rescan_keep|col/vacuum/parallel_baseline|col/vacuum/parallel_keys|table/f2/plans)|fast-gate-assert$)'
# the tests the assertions build leaves to the other builds: the server releases each catalog entry at its last unpin (CATCACHE_FORCE_RELEASE), which makes every planning and every
# jsonb_build_object call 40 to 100 times slower, and they plan hundreds of generated statements or load 100,000 documents. Two of them took 557 to 729 s (run 40193282663) and the
# six others 122 to 249 s there, 3 to 26 s everywhere else; the planner paths they reach stay in the family through the tests beside them (agg_cte_inline_upper, agg_cte_hoist...)
ASSERT_SKIP='^installcheck-(agg/cte/inline_band|agg/cte/inline_rows|agg/cte/scope_lineage|agg/flat_rewrites_consolidated|agg/flat_rewrites_consolidated_2|agg/join/flip_probe_consolidated|agg/natural_cost|agg/two_level_rows_consolidated_2)$'
# the Format 1 lane: the fast gate on Format 1 and the upgrade tests of the v5 formats (SIFT_CI_JOBS='$COMPAT_F1_JOBS' runs just those)
COMPAT_F1_JOBS='^(compat-f1-fast-gate|tap-(compat/v51_full)|tap-(compat/v6_dump_restore_tablespace))$'
# What a push runs on the compatibility entries (PostgreSQL 17, PostgreSQL 19, AArch64), the one shard each: what depends on the major or the architecture, the regression tests
# (the SQL surface, the planner and executor hooks, the NEON kernels), the isolation specs, the crash, standby and upgrade tests, the corpus and the plan bands. The differential tests, the fuzzers,
# the soaks and the slopes run on the PostgreSQL 18 x86 entry
COMPAT_JOBS='^(installcheck-|isolation-[0-9]+$|isolation-emulated$|tap-(agg/lookup_shared_concurrent|bm25/crash|bm25/parallel_crash|bm25/standby|bm25/wal_minimal|col/f2/minimal_wal|col/f2/wal|compat/v51_full|compat/v6_dump_restore_tablespace|table/basic|table/logical)$|tap-shell-(bm25/index/build_crash|bm25/store/crash|bm25/store/replica|col/crash_matrix|col/dict/shared_shm|col/index_crash|col/index_replica|compat/pg_upgrade/basic|core/leakcheck_gate|core/replication|fault/matrix-smoke|maint/standby|table/load_workers|table/replica_mixed)$|plan-gate(-table)?$|corpus$|difftest-col(-tbl|-selftest)?$|isa-baseline$|tools-build$|simd-level$)'
# the entry of PostgreSQL 19 runs all of it; 17 has no pg_upgrade job (it upgrades from the major before 18), and AArch64 no mixed-version replication job, which builds the older release it replicates
# from: only the PostgreSQL 18 x86 entry reads the history of the release tags
COMPAT_JOBS_17=${COMPAT_JOBS/compat\/pg_upgrade\/basic|/} COMPAT_JOBS_ARM=${COMPAT_JOBS_17/|table\/replica_mixed)/)}
if [[ $MODE == affected ]]; then
	shift
	OUT=pick
	FOCUS=
	[[ ${1:-} == --focus ]] && { FOCUS=1; shift; }
	[[ ${1:-} == --list || ${1:-} == --github ]] && { OUT=${1#--}; shift; }
	BASE=${1:-origin/main}
	if [[ $BASE == all ]]; then RE=. FASTRE=.
	elif [[ $BASE == --files ]]; then   # for tests of the map: the jobs of named files (--focus: FILE:FUNCTION tokens)
		if [[ -n $FOCUS ]]; then RE=$(focus_regex "${@:2}") || exit $?; FASTRE=$RE
		else
			FILES=("${@:2}")
			RE=$(areas_regex "${@:2}") || exit $?
			FASTRE=$(areas_fast_regex "${@:2}") || exit $?
		fi
	else
		MB=$(git -C "$REPO" merge-base "$BASE" HEAD) || exit 1
		# an untracked path no row of the map covers is another lane's scratch (a worktree or a log in a shared tree), not a change of this branch; a tracked file with no row stays an error
		mapfile -t FILES < <(cd "$REPO" && { git diff --name-only --no-renames "$MB"; git ls-files --others --exclude-standard | while IFS= read -r f; do [[ -z $(areas_unmatched "$f") ]] && echo "$f"; done; } | sort -u)
		# a diff of more than SIFT_CI_WIDE_DIFF files (a pull request of a whole release line: 1,841 files) picks every job: the regex of its files is tens of kB and matching it against the
		# names of the jobs, the tests and the fast tier took 146 s on a quiet machine, over the 5 minutes of the `changes` job on a runner
		if (( ${#FILES[@]} > ${SIFT_CI_WIDE_DIFF:-300} )); then RE=. FASTRE=.
		else
			RE=$(areas_regex "${FILES[@]}") || exit $?
			FASTRE=$(areas_fast_regex "${FILES[@]}") || exit $?
		fi
		if [[ -n $FOCUS && $RE != . ]]; then
			# the functions of the changed files of src/ from the diff (a file with no tokens is new: its callers are the other tokens), the other files as themselves
			mapfile -t TOK < <({ printf '%s\n' "${FILES[@]}" | grep -v '^src/'; focus_tokens "$MB"; } | sort -u)
			RE=$(focus_regex ${TOK[@]+"${TOK[@]}"}) || exit $?
			FASTRE=$RE
		fi
	fi
	# the canaries of an area (scripts/perf/gate.py) are a few seconds each: a pull request runs the ones its files are mapped to
	[[ $FASTRE == . ]] || FASTRE+='|^perf-gate-'
	mapfile -t JOBS_ALL < <(env -u SIFT_CI_JOBS -u SIFT_CI_AFFECTED -u SIFT_CI_SHARD SIFT_CI_AFFECTED="$RE" SIFT_CI_PG_MAJOR=18 "$0" --plan quick | cut -d' ' -f1)
	PICK=()
	for f in $(tests sql .sql); do [[ installcheck-$f =~ $RE ]] && BATCHES=1; done
	for id in "${JOBS_ALL[@]}"; do [[ $id == installcheck-batch* && -n ${BATCHES:-} || $id =~ $RE ]] && PICK+=("$id"); done
	flag() { [[ gh:$1 =~ $RE ]] && echo true || echo false; }
	cassert=false
	for f in ${FILES[@]+"${FILES[@]}"}; do [[ $f == src/*.c && -f $REPO/$f ]] && grep -qE "$ASSERT_MARK" "$REPO/$f" && cassert=true; done
	# the fast tier of a pull request (SIFT_CI_JOBS): the regression tests, the cheap checks, and the tests the diff itself touches; the rest runs on main
	FAST='^(installcheck-|isolation-emulated$|coverage$|tools-build$|format-isolation$|perf-gate-|tap-shell-(agg/viability_gate)$)'
	for f in ${FILES[@]+"${FILES[@]}"}; do
		if [[ $f =~ ^test/t/(.+)\.(pl|sh)$ ]]; then FAST+="|^tap(-shell)?-${BASH_REMATCH[1]}\$"
		elif [[ $f == test/isolation/* ]]; then FAST+='|^isolation-[0-9]+$'
		elif [[ $f == bench/scaling/* || $f == scripts/scaling-gate.sh ]]; then FAST+='|^tap-shell-core/scaling_slope$'
		elif [[ $f == test/col/* ]]; then FAST+="|^difftest-col|${GROUP[plan_gate]}"
		elif [[ $f == test/fuzz/diff* ]]; then FAST+='|^difftest-fuzz(-tbl)?(-[12])?$'
		elif [[ $f == test/fuzz/txn* ]]; then FAST+='|^difftest-txn(-shapes)?$'
		elif [[ $f == test/fuzz/bm25* ]]; then FAST+='|^difftest-bm25$'
		elif [[ $f == test/fuzz/shapes_matrix* || $f == test/fuzz/surface_sweep* ]]; then FAST+='|^shapes-(matrix|surface)$'
		elif [[ $f == test/plan/* || $f == scripts/plangate.sh ]]; then FAST+="|${GROUP[plan_gate]}"; fi
	done
	# a pull request builds its fast tier only when a job it can run is picked: scripts/areas.tsv may pick one of the rest, and no job selected is an error of the run
	fastpg=false
	for id in "${PICK[@]}"; do [[ $id =~ $FAST && ( $id == installcheck-batch* || $id =~ $FASTRE ) ]] && fastpg=true; done
	case $OUT in
		list) echo "${#PICK[@]} of ${#JOBS_ALL[@]} jobs: ${PICK[*]}"; for f in sqlite mariadb replication macos musl f1; do echo "gh:$f $(flag $f)"; done ;;
		github)
			echo "jobs=$RE"
			echo "fast=$FAST"
			echo "fastjobs=$FASTRE"
			echo "fastpg=$fastpg"
			echo "cassert=$cassert"
			echo "cassertjobs=$ASSERT_JOBS"
			echo "compat=$COMPAT_JOBS"
			echo "compat17=$COMPAT_JOBS_17"
			echo "compatarm=$COMPAT_JOBS_ARM"
			echo "pg=$( (( ${#PICK[@]} )) && echo true || echo false)"
			for f in sqlite mariadb replication macos musl f1; do echo "$f=$(flag $f)"; done
			echo "hardened=$([[ " ${PICK[*]} " == *' hardened '* ]] && echo true || echo false)"
			echo "formula=$([[ $BASE == all || " ${FILES[*]-} " == *' Formula/pg_sift.rb '* ]] && echo true || echo false)"
			echo "nshards=$( (( ${#PICK[@]} >= ${SIFT_CI_SHARD_MIN:-40} )) && echo 3 || echo 1)"
			echo "full=$( (( ${#PICK[@]} == ${#JOBS_ALL[@]} )) && [[ $(flag sqlite)$(flag mariadb)$(flag replication) == truetruetrue ]] && echo true || echo false)" ;;
		pick) "$REPO/scripts/check-neutral.sh" || exit 1   # a change of any file: the wording guard is not a job of the map
			(( ${#PICK[@]} )) || { echo "nothing to run: the diff touches no job"; exit 0; }
			echo "ci: ${#PICK[@]} of ${#JOBS_ALL[@]} jobs" >&2
			SIFT_CI_AFFECTED=$RE exec "$0" quick ;;
	esac
	exit 0
fi
