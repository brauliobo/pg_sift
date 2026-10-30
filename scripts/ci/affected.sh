# The files a change touched, mapped to the jobs it can break: `affected`, `--areas-regex`, the focus tokens and the assertions family.
# The CI workflow and `affected` share scripts/areas.tsv through it; it never starts a suite. Sourced by scripts/ci.sh; not a script of its own.
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
# focus_regex TOKEN...: the jobs of `affected --focus`. A token is FILE or FILE:FUNCTION (FUNCTION * : a change outside any function); the regression tests that
# execute the function (scripts/focus.tsv, made by scripts/focus-gen.sh), the tests of the area for a function every test executes (the =hot row) and for a
# change outside functions, scripts/areas.tsv as ever for the other files. A function no row has is new or run by no test: its callers are the other tokens.
focus_regex() {
	local t f rest=() star=() sel names
	for t; do
		f=${t%%:*}
		[[ $f =~ ^src/(agg|col|table|bm25)/([^/]+/)?[^/]+\.[ch]$ ]] || { rest+=("$f"); continue; }
		[[ $t != *:'*' ]] || star+=("$f")
	done
	sel=$(awk -F'\t' -v toks="$*" 'BEGIN { n = split(toks, a, " "); for (i = 1; i <= n; i++) want[a[i]] = 1 }
		$1 == "=hot" { n = split($2, h, " "); for (i = 1; i <= n; i++) if (h[i] in want) { split(h[i], p, ":"); print "H " p[1] }; next }
		/^#/ { next }
		{ n = split($2, h, " "); for (i = 1; i <= n; i++) if (h[i] in want) { print "T " $1; break } }' "$REPO/scripts/focus.tsv")
	# the tests pinned to a file (scripts/focus-pins.tsv): they print numbers of its functions, which a function every test executes (the =hot row) does not name
	sel+=$'\n'$(awk -F'\t' -v toks="$*" 'BEGIN { n = split(toks, a, " "); for (i = 1; i <= n; i++) { split(a[i], p, ":"); want[p[1]] = 1 } }
		/^#/ { next }
		$1 in want { n = split($2, t, " "); for (i = 1; i <= n; i++) print "T " t[i] }' "$REPO/scripts/focus-pins.tsv")
	names=$(sed -n 's/^T //p' <<<"$sel" | paste -sd'|' -)
	star+=($(sed -n 's/^H //p' <<<"$sel" | sort -u))
	{ echo '^(coverage|isolation-emulated)$'
	  [[ -z $names ]] || echo "^installcheck-($names)\$"
	  (( ! ${#star[@]} )) || areas_fast_regex "${star[@]}"
	  (( ! ${#rest[@]} )) || areas_regex "${rest[@]}"
	} | paste -sd'|' -
}
# focus_tokens BASE: FILE:FUNCTION of every hunk of the diff against BASE in src/; the function is the last line at column 0 that starts like a definition before the hunk
focus_tokens() {
	local f l
	for f in $(cd "$REPO" && git diff --name-only --no-renames "$1" -- src); do
		[[ -f $REPO/$f ]] || continue
		git -C "$REPO" diff -U0 --no-renames "$1" -- "$f" | sed -n '/^@@ -0,0 /d; s/^@@ [^+]*+\([0-9]*\).*/\1/p' | while read -r l; do
			awk -v f="$f" -v l="$l" 'NR <= l && /^[A-Za-z_][A-Za-z0-9_]*\(/ { n = $0; sub(/\(.*/, "", n) } END { print f ":" (n == "" ? "*" : n) }' "$REPO/$f"
		done
	done | sort -u
}
# The assertions family of a pull request (the `cassert` output of `affected --github`, run on the PostgreSQL 18 built with --enable-cassert): a change of a source that starts
# workers or handles snapshots or executor hooks (ASSERT_MARK, read from the file: no list to keep) runs the regression tests that run those paths. The `assertions` label
# of a pull request runs the family for any change. A change reached the tip with green fast checks and failed an Assert of the server (GetActiveSnapshot) in a worker.
# Measured (run 40164658605, 27 tests, 4 lanes): the job 8.0 min (cache 1 s, extension build 34 s, tests 7.2 min wall, scratch 435 MB); the slowest tests agg_preagg_elem_parallel 266 s,
# agg_two_level_spine 220 s. It found an open assertion on its first run. The tests are not in the weights below: they are not sharded (one runner).
ASSERT_MARK='RestoreSnapshot|PushActiveSnapshot|GetActiveSnapshot|ParallelWorkerMain|InitializeParallelDSM|shm_toc_|CreateQueryDesc|ExecutorStart_hook'
ASSERT_JOBS='^installcheck-(agg_(par_|subplan|two_level|lookup_shared|.*parallel)|aggstub_parallel|col_.*parallel|parallel_)'
# the tests the assertions build leaves to the other builds: they load 130,000 documents into a sift_col index and compare cost ratios, 557 to 729 s each there (run 40193282663),
# 18 to 26 s everywhere else, and the planner paths they reach (agg_cte_inline_upper, _nested, agg_cte_hoist...) stay in the family
ASSERT_SKIP='^installcheck-agg_cte_inline_(horizon|band|rows)$'
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
		mapfile -t FILES < <(cd "$REPO" && { git diff --name-only --no-renames "$MB"; git ls-files --others --exclude-standard; } | sort -u)
		RE=$(areas_regex "${FILES[@]}") || exit $?
		FASTRE=$(areas_fast_regex "${FILES[@]}") || exit $?
		if [[ -n $FOCUS ]]; then
			# the functions of the changed files of src/ from the diff (a file with no tokens is new: its callers are the other tokens), the other files as themselves
			mapfile -t TOK < <({ printf '%s\n' "${FILES[@]}" | grep -v '^src/'; focus_tokens "$MB"; } | sort -u)
			RE=$(focus_regex ${TOK[@]+"${TOK[@]}"}) || exit $?
			FASTRE=$RE
		fi
	fi
	mapfile -t JOBS_ALL < <(env -u SIFT_CI_JOBS -u SIFT_CI_AFFECTED -u SIFT_CI_SHARD SIFT_CI_PG_MAJOR=18 "$0" --plan quick | cut -d' ' -f1)
	PICK=()
	for f in "$REPO"/test/sql/*.sql; do f=${f##*/}; [[ installcheck-${f%.sql} =~ $RE ]] && BATCHES=1; done
	for id in "${JOBS_ALL[@]}"; do [[ $id == installcheck-batch* && -n ${BATCHES:-} || $id =~ $RE ]] && PICK+=("$id"); done
	flag() { [[ gh:$1 =~ $RE ]] && echo true || echo false; }
	cassert=false
	for f in ${FILES[@]+"${FILES[@]}"}; do [[ $f == src/*.c && -f $REPO/$f ]] && grep -qE "$ASSERT_MARK" "$REPO/$f" && cassert=true; done
	# the fast tier of a pull request (SIFT_CI_JOBS): the regression tests, the cheap checks, and the tests the diff itself touches; the rest runs on main
	FAST='^(installcheck-|isolation-emulated$|coverage$|tools-build$)'
	for f in ${FILES[@]+"${FILES[@]}"}; do
		if [[ $f =~ ^test/t/(.+)\.(pl|sh)$ ]]; then FAST+="|^tap(-shell)?-${BASH_REMATCH[1]}\$"
		elif [[ $f == test/isolation/* ]]; then FAST+='|^isolation$'
		elif [[ $f == bench/scaling/* || $f == scripts/scaling-gate.sh ]]; then FAST+='|^tap-shell-scaling_slope$'
		elif [[ $f == test/col/* ]]; then FAST+='|^difftest-col|^plan-gate(-tuned|-calibrate(-tuned)?|-table(-tuned)?)?$'
		elif [[ $f == test/fuzz/diff_fuzz* ]]; then FAST+='|^difftest-fuzz(-tbl)?(-[12])?$'
		elif [[ $f == test/plan/* || $f == scripts/plangate.sh ]]; then FAST+='|^plan-gate(-tuned|-calibrate(-tuned)?|-table(-tuned)?)?$'; fi
	done
	# a pull request builds its fast tier only when a job it can run is picked: scripts/areas.tsv may pick one of the rest, and no job selected is an error of the run
	fastpg=false
	for id in "${PICK[@]}"; do [[ $id =~ $FAST && ( $id == installcheck-batch* || $id =~ $FASTRE ) ]] && fastpg=true; done
	case $OUT in
		list) echo "${#PICK[@]} of ${#JOBS_ALL[@]} jobs: ${PICK[*]}"; for f in sqlite mariadb replication macos; do echo "gh:$f $(flag $f)"; done ;;
		github)
			echo "jobs=$RE"
			echo "fast=$FAST"
			echo "fastjobs=$FASTRE"
			echo "fastpg=$fastpg"
			echo "cassert=$cassert"
			echo "cassertjobs=$ASSERT_JOBS"
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
