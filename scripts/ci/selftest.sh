# `--selftest`: checks the plan of every PostgreSQL major, the shards, the areas coverage, the loud failures and the reaper.
# Runs instead of a suite and exits with the number of failed checks. Sourced by scripts/ci.sh; not a script of its own.
# --plan [quick|full]: the job plan (job, suite, pool or after, command) without running anything; --selftest: checks the plan of every
# PostgreSQL major (SIFT_CI_PG_MAJOR overrides the server's) against the rule that no job installs a library flavor while the lanes run
if [[ $MODE == --selftest ]]; then
	bad=0
	unset SIFT_CI_JOBS SIFT_CI_AFFECTED SIFT_CI_SHARD SIFT_CI_DEEP   # a run of ci.sh that selected jobs starts this check (lint) with them set: the plans below are the whole plans
	# plan_of MODE PG [DEEP [SHARD]]: sets PLAN_OUT to that plan; each distinct plan is made once (a run of --plan costs a second of CPU and the checks below ask for the same ones many times)
	ST=$(mktemp -d); trap 'rm -rf "$ST"' EXIT   # everything the selftest makes is under it
	declare -A PLANS INSTALLS
	installs() { [[ -n ${INSTALLS[$1]+x} ]] || { grep -E 'pgtest\.sh"? install' "$1" | grep -qvE '^\s*#|echo' && INSTALLS[$1]=0 || INSTALLS[$1]=1; }; return "${INSTALLS[$1]}"; }
	before=" $(descendants $$ | tr '\n' ' ')"
	plan_of() {
		local k="$1 $2 ${3:-} ${4:-}" out i n
		if [[ -z ${PLANS[$k]+x} && ${4:-} =~ ^[0-9]+/([0-9]+)$ ]]; then   # a shard: the plans of all its shards in one run
			n=${BASH_REMATCH[1]}
			out=$(env SIFT_CI_DEEP="${3:-}" SIFT_CI_SHARD=all/$n SIFT_CI_PG_MAJOR="$2" "$0" --plan full) || return 1
			for (( i = 1; i <= n; i++ )); do PLANS["full $2 ${3:-} $i/$n"]=$(awk -v k=$i '$1 == k { sub(/^[0-9]+ /, ""); print }' <<<"$out"); done
		fi
		if [[ -z ${PLANS[$k]+x} ]]; then
			out=$(env SIFT_CI_DEEP="${3:-}" ${4:+SIFT_CI_SHARD=$4} SIFT_CI_PG_MAJOR="$2" "$0" --plan "$1") || return 1
			PLANS[$k]=$out
		fi
		PLAN_OUT=${PLANS[$k]}
	}
	for pg in 16 17 18; do
		for m in quick full; do
			plan_of $m $pg || { echo "FAIL: --plan $m on PostgreSQL $pg"; bad=1; continue; }
			plan=$PLAN_OUT
			n=0
			while read -r id suite where cmd; do
				# a job whose script runs `pgtest.sh install` builds a flavor of its own: before 18 that is the server's library
				for w in $cmd; do [[ $w == */scripts/*.sh && -f $w ]] && installs "$w" && break || w=; done
				[[ -n $w ]] || continue
				n=$((n + 1))
				want=pool; (( pg < 18 )) && want=after
				[[ $where == "$want" ]] || { echo "FAIL: PostgreSQL $pg $m: $id installs a flavor and is placed $where, want $want"; bad=1; }
			done <<<"$plan"
			# the lean full tier installs its flavor (hardened) on 18 only
			(( n )) || { [[ $m == full ]] && (( pg < 18 )); } || { echo "FAIL: PostgreSQL $pg $m: the plan has no job that installs a flavor (the check is void)"; bad=1; }
		done
	done
	# scripts/areas.tsv: every tracked file is in a row, and every job of the plans (batches of short tests are cut from the tests of the areas) is in a group, so that
	# neither a file nor a test is left out of the selection; a core file selects everything
	mapfile -t tracked < <(git -C "$REPO" ls-files)
	mapfile -t unmatched < <(areas_unmatched "${tracked[@]}")
	miss=$(areas_invalid; (( ! ${#unmatched[@]} )) || areas_regex "${unmatched[@]}" 2>&1 >/dev/null)   # a file in no row is named by areas_regex
	[[ -z $miss ]] || { echo "FAIL: $miss"; bad=1; }
	# the table of rows that areas_invalid must flag (a path, a jobs expression) and the ones it must not (a group, a capture)
	t=$ST/areas.tsv
	printf '=g\tinstallcheck-a\nsrc/x/(.*)\t{g}|\\1\nsrc/(\t.\nsrc/y\t(\nsrc/z\t-\nsrc/w\tfast:{g}\n' >"$t"
	areas_load "$t"
	[[ $(areas_invalid | grep -c 'invalid ERE') == 2 ]] || { echo "FAIL: areas_invalid does not flag exactly the two invalid rows: $(areas_invalid)"; bad=1; }
	printf 'src/x/.*\t.\nsrc/y\t-\n' >"$t"
	areas_load "$t"
	[[ $(areas_unmatched src/x/a src/y other) == other ]] || { echo "FAIL: areas_unmatched does not name the one file no row matches"; bad=1; }
	areas_load "$REPO/scripts/areas.tsv"
	grouped=$(printf '(%s)|' "${GROUP[@]}")
	for m in quick full; do
		plan_of $m 18
		while read -r id _; do
			[[ $id == installcheck-batch* || $id =~ ${grouped%|} ]] || { echo "FAIL: areas: the $m job $id is in no group of scripts/areas.tsv"; bad=1; }
		done <<<"$PLAN_OUT"
	done
	for f in "$REPO"/test/sql/*.sql; do
		f=${f##*/}
		[[ installcheck-${f%.sql} =~ ${grouped%|} ]] || { echo "FAIL: areas: the test $f is in no group of scripts/areas.tsv"; bad=1; }
	done
	# a script of scripts/ selects every job that runs it, and a change of documentation, benchmarks or release notes selects none
	for m in quick full; do
		plan_of $m 18
		while read -r id _ _ cmd; do
			eval "args=($cmd)"
			for a in "${args[@]}"; do
				[[ $a == "$REPO"/scripts/* ]] || continue
				rx=$(areas_regex "${a#"$REPO"/}")
				[[ -n $rx && $id =~ $rx ]] || { echo "FAIL: areas: ${a#"$REPO"/} runs the $m job $id and a change of it does not select the job"; bad=1; }
			done
		done <<<"$PLAN_OUT"
	done
	[[ $(areas_regex docs/USAGE.md CHANGELOG.md README.md bench/README.md) == '^$' ]] || { echo "FAIL: areas: a change of documentation or benchmarks selects jobs"; bad=1; }
	# the focus table (scripts/focus.tsv, made by scripts/focus-gen.sh): a row for every regression test and none for a test that is gone; the lookup selects the
	# tests that execute a function, the tests of the area for a hot function or a change outside functions
	focus_rows=$(cut -f1 "$REPO/scripts/focus.tsv" | grep -v '^[#=]' | sort)
	focus_missing=$(comm -23 <(ls "$REPO/test/sql" | sed 's/\.sql$//' | sort) <(echo "$focus_rows"))
	[[ -z $focus_missing ]] || { echo "FAIL: focus: no row in scripts/focus.tsv for $(echo $focus_missing) (scripts/focus-gen.sh NAME...)"; bad=1; }
	focus_stale=$(comm -13 <(ls "$REPO/test/sql" | sed 's/\.sql$//' | sort) <(echo "$focus_rows"))
	[[ -z $focus_stale ]] || { echo "FAIL: focus: scripts/focus.tsv has rows for $(echo $focus_stale), which are not tests"; bad=1; }
	read -r focus_t focus_tok _ < <(awk -F'\t' '!/^[#=]/ && $2 != "" { split($2, a, " "); print $1, a[1]; exit }' "$REPO/scripts/focus.tsv")
	[[ -z $focus_t || installcheck-$focus_t =~ $(focus_regex "$focus_tok") ]] || { echo "FAIL: focus: $focus_tok does not select its test $focus_t"; bad=1; }
	focus_hot=$(awk -F'\t' '$1 == "=hot" { split($2, a, " "); print a[1] }' "$REPO/scripts/focus.tsv")
	[[ -z $focus_hot || $(focus_regex "$focus_hot") == "$(focus_regex "${focus_hot%%:*}:*")" ]] || { echo "FAIL: focus: the hot function $focus_hot does not select the tests of its area"; bad=1; }
	[[ $(focus_regex src/agg/cte/hook.c:no_such_function) == '^(coverage|isolation-emulated)$' ]] || { echo "FAIL: focus: a function no row has selects tests"; bad=1; }
	# the assertions family of a pull request: the tests that run workers exist, and the file that starts them is a trigger
	n=0; for f in "$REPO"/test/sql/*.sql; do f=${f##*/}; [[ installcheck-${f%.sql} =~ $ASSERT_JOBS ]] && n=$((n + 1)); done
	(( n >= 20 )) || { echo "FAIL: the assertions family ASSERT_JOBS selects $n regression tests, want 20 or more"; bad=1; }
	grep -qE "$ASSERT_MARK" "$REPO/src/agg/par.c" || { echo "FAIL: src/agg/par.c, which starts workers, is no trigger of the assertions family (ASSERT_MARK)"; bad=1; }
	# the fast tier: a source file runs the tests of its own area (src/agg: not the ones only the table area owns), a core file all of them
	fast_agg=$(areas_fast_regex src/agg/agg.c) fast_core=$(areas_fast_regex src/core/ops.c)
	plan_of quick 18
	while read -r id _; do
		[[ $id == installcheck-* ]] || continue
		[[ $id =~ $fast_core ]] || { echo "FAIL: areas: src/core/ops.c does not select the fast job $id"; bad=1; }
		[[ $id =~ ${GROUP[tbl]} && ! $id =~ ${GROUP[agg]} && ! $id =~ ${GROUP[col]} && $id =~ $fast_agg ]] && { echo "FAIL: areas: a src/agg file selects the table-only fast job $id"; bad=1; }
		[[ $id =~ ${GROUP[agg]} && ! $id =~ $fast_agg ]] && { echo "FAIL: areas: a src/agg file does not select the agg fast job $id"; bad=1; }
	done <<<"$PLAN_OUT"
	for f in src/core/ops.c "$REPO"/sql/*.sql; do
		f=${f#"$REPO"/}
		[[ $(areas_regex "$f") == *'|.' ]] || { echo "FAIL: areas: $f does not select every job"; bad=1; }
	done
	for deep in '' 1; do
		cs=$COL_SLICES_LEAN; [[ -z $deep ]] || cs=$COL_SLICES_DEEP
		# the column differential jobs of the full tier are slices of one corpus (lean 4, deep 12): K/N for K = 1..N, nothing else different between them (no subset of the variants,
		# scenarios or queries), each slice has queries, no query is in two, and together they are the corpus the unsliced script selects. The lean slices are split between 17 and
		# 18 (each once), the deep ones are all on 18
		plan=
		for pg in 18 $([[ -n $deep ]] || echo 17); do plan_of full $pg "$deep"; plan+=$PLAN_OUT$'\n'; done
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
			[[ $(tr ' ' '\n' <<<"$ks" | grep . | sort -n | tr '\n' ' ') == "$(seq 1 "$cs" | tr '\n' ' ')" ]] || { echo "FAIL: the slices of $prefix are not 1..$cs:$ks"; bad=1; }
			if [[ -n $cd ]]; then
				whole=$(cd "$REPO/test/col/fixtures" && printf '%s\n' *.sql)   # the directory, not coldiff.sh's own idea of it
				[[ -z $(sort <<<"$union" | grep . | uniq -d) ]] || { echo "FAIL: a query is in two slices of $prefix: $(sort <<<"$union" | grep . | uniq -d | head -3 | tr '\n' ' ')"; bad=1; }
				[[ $(sort <<<"$union" | grep .) == "$(sort <<<"$whole")" ]] || { echo "FAIL: the slices of $prefix are not the whole corpus ($(grep -c . <<<"$union") of $(grep -c . <<<"$whole") queries)"; bad=1; }
			fi
		done
		grep -q '^difftest-col-analytics ' <<<"$plan" || { echo "FAIL: the full plan ${deep:+(deep) }lacks difftest-col-analytics (the analytics suite is not in the slices)"; bad=1; }
	done
	# before PostgreSQL 18 the full tier runs half of the slices: the first and second of 4 (lean; 18 runs the others), the first nine of 18 (deep; 18 runs all 12)
	for pg in 16 17; do
		for deep in '' 1; do
			plan_of full $pg "$deep"
			n=$(grep -cE '^difftest-col-[0-9]+ ' <<<"$PLAN_OUT")
			(( n == (deep ? COL_SLICES_DEEP17 : COL_SLICES_LEAN) / 2 )) || { echo "FAIL: PostgreSQL $pg ${deep:+deep }full plan has $n slices of difftest-col"; bad=1; }
		done
	done
	# the smoke of the release library (release_library_tests): each name is a test the release build runs, or the quick tier would silently drop it
	plan_of quick 18
	prod=$(grep '^release-library-regress ' <<<"$PLAN_OUT")
	for t in $STOCK_SMOKE; do [[ " $prod " == *" $t "* ]] || { echo "FAIL: $t of STOCK_SMOKE is not in the release-library-regress job (no test of that name, a slow or test-only one)"; bad=1; }; done
	n=0; for f in "$REPO"/test/sql/*.sql; do f=${f##*/}; [[ installcheck-${f%.sql} =~ $ASSERT_SKIP ]] && n=$((n + 1)); done
	(( n == 3 )) || { echo "FAIL: ASSERT_SKIP selects $n regression tests, want the 3 it names"; bad=1; }
	# the shards of the full tier: every job once, and the load by the weights within 15% of the least loaded shard
	for pg in 17 18; do
	for deep in '' 1; do
		ns=$("$0" --shards "$pg" "$deep")
		plan_of full $pg "$deep"
		all=$(awk '{print $1}' <<<"$PLAN_OUT" | sort)
		got=
		for (( k = 1; k <= ns; k++ )); do plan_of full $pg "$deep" $k/$ns; got+=$PLAN_OUT$'\n'; done
		got=$(awk 'NF {print $1}' <<<"$got" | sort)
		[[ $all == "$got" ]] || { echo "FAIL: the $ns shards of the full tier on PostgreSQL $pg ${deep:+deep }are not the plan, each job once"; bad=1; }
		lo='' hi=0
		for (( k = 1; k <= ns; k++ )); do
			l=0
			plan_of full $pg "$deep" $k/$ns
			while read -r j _; do weight_of "$j" "$deep" "$pg"; l=$(( l + WEIGHT )); done <<<"$PLAN_OUT"
			(( l > hi )) && hi=$l
			[[ -z $lo ]] || (( l < lo )) && lo=$l
		done
		(( hi * 100 <= lo * 115 )) || { echo "FAIL: the shards of the full tier on PostgreSQL $pg ${deep:+deep }are unbalanced by the weights of scripts/ci.sh (heaviest $hi s, lightest $lo s)"; bad=1; }
	done
	done
	# the plans of the shards made all at once (SIFT_CI_SHARD=all/N) are those of a run for the shard alone, for a shard of the pool and the last one (it runs the jobs after the pool)
	for k in 3 9; do
		plan_of full 18 1 $k/9
		[[ $PLAN_OUT == "$(env SIFT_CI_DEEP=1 SIFT_CI_SHARD=$k/9 SIFT_CI_PG_MAJOR=18 "$0" --plan full)" ]] || { echo "FAIL: the plan of shard $k/9 made with all the shards differs from the plan of that shard alone"; bad=1; }
	done
	# a released extension script is never edited (an install upgraded in place would miss the change): new objects go into the next version's scripts
	(cd "$REPO" && scripts/sha256.sh --quiet -c scripts/released-sql.sha256) || { echo "FAIL: a released sql/pg_sift--*.sql differs from its tag; add the change to the scripts of the next version (a release appends its own scripts to scripts/released-sql.sha256)"; bad=1; }
	# scripts/leftovers.allow: a test that exists, an object and a reason on every line
	re='^([a-z0-9_]+): .+ # .+$'
	while IFS= read -r l; do
		[[ -z $l || $l == '#'* ]] && continue
		[[ $l =~ $re && -f $REPO/test/sql/${BASH_REMATCH[1]}.sql ]] || { echo "FAIL: scripts/leftovers.allow: \"$l\" is not \"test: regex # reason\" of an existing test"; bad=1; }
	done < "$REPO/scripts/leftovers.allow"
	# bash 3.2 (stock macOS) exits 0 on the syntax of bash 4: a script that uses it starts with the check that stops it with a message
	b4='declare -A|local -A|local -n|declare -n|mapfile|readarray|\[\[ -v |\$\{[A-Za-z_]+(,,|\^\^)\}|wait -n|EPOCHSECONDS|EPOCHREALTIME|printf .%\(|&>>|\|&|coproc|;;&|read -[a-zA-Z]*N'
	for f in $(git -C "$REPO" ls-files 'scripts/*.sh' 'test/t/*.sh' 'test/t/*.bash'); do
		[[ $f == scripts/ci/* ]] && continue   # the parts scripts/ci.sh sources: its first command is the check
		! grep -qE "$b4" "$REPO/$f" || grep -q 'needs bash 4+' "$REPO/$f" || { echo "FAIL: $f uses bash 4 and lacks the first-line check of scripts/ci.sh"; bad=1; }
	done
	# a script that makes a temporary file or directory removes it on the way out, also when it is stopped (8,552 entries of /tmp/tmp.* were left by lint and ci helpers)
	for f in $(git -C "$REPO" ls-files 'scripts/*.sh'); do
		! grep -qE '^[^#]*\bmktemp\b' "$REPO/$f" || grep -qE '^[^#]*\btrap .*\b(EXIT|RETURN)\b' "$REPO/$f" || { echo "FAIL: $f calls mktemp and sets no EXIT trap that removes it"; bad=1; }
	done
	# the checksum tool of macOS is BSD's (`-c --quiet` needs a file operand) and older releases have none: scripts/sha256.sh is the one caller (the pattern is split so that this line is not a match)
	h='sha256''sum|md5''sum'
	for f in $(git -C "$REPO" ls-files 'scripts/*.sh' 'test/t/*.sh' 'test/t/*.bash'); do
		[[ $f == scripts/sha256.sh ]] || ! grep -qE "\\b($h)\\b" "$REPO/$f" || { echo "FAIL: $f calls the system checksum tool; use scripts/sha256.sh"; bad=1; }
	done
	# the other tools of macOS are BSD's too: constructs GNU accepts that they reject or print differently, one row per pattern and the portable spelling. The Linux-only scripts and this file (it holds the patterns) are exempt
	bsd=(
		'stat (-c|--format)	a size: wc -c <FILE | tr -d " "'
		"paste -sd('[^']*'|[^ ']*)([|);\"]|\$)	paste wants a file operand: paste -sd, -"
		'seq -s	BSD seq ends the list with the separator: seq 1 N | tr "\n" " "'
		'pg_sift\.so	the module of macOS is pg_sift.dylib: pg_sift$DLSUFFIX (test/t/lib.bash)'
	)
	for row in "${bsd[@]}"; do
		IFS=$'\t' read -r re why <<<"$row"
		for f in $(git -C "$REPO" ls-files 'scripts/*.sh' 'test/t/*.sh' 'test/t/*.bash'); do
			case $f in scripts/ci.sh|scripts/ci/selftest.sh|scripts/apt-install.sh|scripts/isa_check.sh|scripts/release.sh) continue ;; esac
			! grep -qE "$re" "$REPO/$f" || { echo "FAIL: $f: /$re/ is not portable to macOS; $why"; bad=1; }
		done
	done
	# the python3 of macOS is 3.9: `X | None` in an annotation fails at import there unless the annotations are not evaluated
	for f in $(git -C "$REPO" ls-files '*.py'); do
		! grep -qE '(:|->) *[A-Za-z_.]+(\[[^]]*\])? *\| *None' "$REPO/$f" || grep -q '^from __future__ import annotations' "$REPO/$f" ||
			{ echo "FAIL: $f has an annotation 'X | None' and lacks 'from __future__ import annotations' (Python 3.9)"; bad=1; }
	done
	# the loud failures: bash older than 4 (the check made to fail), and a run that selects no job, stop before anything is built
	# the first command of the script alone, made to fail: a copy of the whole script would run the harness (a copy outside the checkout once left a tree of 41,000 processes)
	first=$(grep -vE '^[[:space:]]*(#|$)' "$0" | head -n 1)
	[[ $first == '((BASH_VERSINFO[0] >= 4))'* ]] || { echo "FAIL: the first command of ci.sh is not the bash 4 check: $first"; bad=1; }
	out=$(bash -c "${first//>= 4/>= 99}" 2>&1) && { echo "FAIL: ci.sh exits 0 on bash older than 4"; bad=1; }
	[[ $out == *'needs bash 4+'* ]] || { echo "FAIL: ci.sh does not say it needs bash 4+: $out"; bad=1; }
	# a copy outside a checkout and a run that has started itself too many times stop before they start anything
	t=$ST/copy.sh; cp "$0" "$t"
	out=$(env -u SIFT_CI_TMP bash "$t" quick 2>&1) && { echo "FAIL: a copy of ci.sh outside a checkout exits 0"; bad=1; }
	[[ $out == *'from the scripts/ of a checkout'* ]] || { echo "FAIL: a copy of ci.sh outside a checkout does not stop with a message: $out"; bad=1; }
	out=$(SIFT_CI_DEPTH=8 "$0" --plan quick 2>&1) && { echo "FAIL: ci.sh exits 0 when it has started itself 8 levels deep"; bad=1; }
	[[ $out == *'levels deep'* ]] || { echo "FAIL: ci.sh does not stop at a depth of 9: $out"; bad=1; }
	# not in the scratch directory and the shard of the run that calls this: a shard may select no job and still builds, in the directory it removes first and at its end
	out=$(env -u SIFT_CI_TMP -u SIFT_CI_SHARD SIFT_CI_JOBS='^none$' "$0" quick 2>&1) && { echo "FAIL: ci.sh exits 0 when no job is selected"; bad=1; }
	[[ $out == *'no test job selected'* ]] || { echo "FAIL: ci.sh does not say that no job is selected: $out"; bad=1; }
	for f in "docs/USAGE.md false" "docs/COL-PLAN.md false" "src/agg/update.c true" "bench/scaling/scaling.py true"; do   # a job the fast tier cannot run does not build it
		fp=$("$0" affected --github --files ${f% *} | grep '^fastpg=')
		[[ $fp == "fastpg=${f#* }" ]] || { echo "FAIL: affected --github for ${f% *} says '$fp', not fastpg=${f#* }"; bad=1; }
	done
	# the reaper: a dead run's directory and its postmaster go, a live run's and a failed run's (kept) stay
	t=$ST/reaper; mkdir "$t"; ( exit 0 ) & dead=$!; wait "$dead"
	mkdir -p "$t/sift-ci-$dead/lane1/data" "$t/sift-ci-$$/lane1" "$t/sift-ci-1$dead" "$t/sift-ci-1$dead/data"
	touch "$t/sift-ci-1$dead/kept"
	( exec -a postgres sleep 60 ) & pm=$!
	printf '%s\n/x\n' "$pm" >"$t/sift-ci-$dead/lane1/data/postmaster.pid"
	reap_stale "$t" 2>/dev/null   # the shell reports the postmaster it killed (a job of this shell)
	[[ ! -e $t/sift-ci-$dead ]] || { echo "FAIL: the directory of a dead run is not reaped"; bad=1; }
	{ wait "$pm"; } 2>/dev/null   # the postmaster is a child of this shell: a stopped one is a zombie until it is waited for
	! kill -0 "$pm" 2>/dev/null || { echo "FAIL: the postmaster in the directory of a dead run is not stopped"; bad=1; kill -KILL "$pm"; }
	[[ -d $t/sift-ci-$$ && -d $t/sift-ci-1$dead ]] || { echo "FAIL: the directory of a live run or a kept one is reaped"; bad=1; }
	# the process count is back to where it started: nothing a check began is left running
	leaked=$(for p in $(descendants $$); do [[ $p == "$BASHPID" || $before == *" $p "* ]] || ps -o pid=,args= -p "$p" 2>/dev/null; done)
	[[ -z $leaked ]] || { echo "FAIL: processes left running by the checks: $leaked"; bad=1; }
	(( bad )) || echo "plan-selftest: no flavor installs while the lanes run (PostgreSQL 16, 17, 18; quick, full); every file and job is in scripts/areas.tsv; the column differential slices are the whole corpus"
	exit $bad
fi
