# shellcheck shell=bash
# `--selftest`: checks the plan of every PostgreSQL major, the shards, the areas coverage, the loud failures and the reaper.
# Runs instead of a suite and exits with the number of failed checks. Sourced by scripts/ci.sh; not a script of its own.
# --plan [quick]: the job plan (job, suite, pool or after, command) without running anything; --selftest: checks the plan of every
# PostgreSQL major (SIFT_CI_PG_MAJOR overrides the server's) against the rule that no job installs a library flavor while the lanes run
# `--selftest [PART...]` runs only those parts (scripts/lint.sh runs each as a check of its own, side by side: the whole takes minutes of one process)
if [[ $MODE == --selftest ]]; then
	bad=0
	ST_PARTS=(flavors areas-files areas-select budgets shards-quick spread portability tooling formats)
	ST_ONLY=("${@:2}")
	st_on() { (( ! ${#ST_ONLY[@]} )) || [[ " ${ST_ONLY[*]} " == *" $1 "* ]]; }
	for p in "${ST_ONLY[@]}"; do [[ " ${ST_PARTS[*]} " == *" $p "* ]] || { echo "ci.sh --selftest: no part $p (${ST_PARTS[*]})"; exit 2; }; done
	unset SIFT_CI_JOBS SIFT_CI_AFFECTED SIFT_CI_SHARD   # a run of ci.sh that selected jobs starts this check (lint) with them set: the plans below are the whole plans
	# plan_of MODE PG|arm [SHARD]: sets PLAN_OUT to that plan; each distinct plan is made once (a run of --plan costs a second of CPU and the checks below ask for the same ones many times)
	ST=$(mktemp -d); trap 'rm -rf "$ST"' EXIT   # everything the selftest makes is under it
	declare -A PLANS INSTALLS
	installs() { [[ -n ${INSTALLS[$1]+x} ]] || { grep -E 'pgtest\.sh"? install' "$1" | grep -vE '^\s*#|echo|log "' >/dev/null && INSTALLS[$1]=0 || INSTALLS[$1]=1; }; return "${INSTALLS[$1]}"; }
	before=" $(descendants $$ | tr '\n' ' ')"
	plan_of() {
		local k="$1 $2 ${3:-}" out i n
		if [[ -z ${PLANS[$k]+x} && ${3:-} =~ ^[0-9]+/([0-9]+)$ ]]; then   # a shard: the plans of all its shards in one run
			n=${BASH_REMATCH[1]}
			out=$(env SIFT_CI_SHARD=all/$n SIFT_CI_PG_MAJOR="${2/arm/18}" FLAVOUR="$2" "$0" --plan "$1") || return 1
			for (( i = 1; i <= n; i++ )); do PLANS["$1 $2 $i/$n"]=$(awk -v k=$i '$1 == k { sub(/^[0-9]+ /, ""); print }' <<<"$out"); done
		fi
		if [[ -z ${PLANS[$k]+x} ]]; then
			out=$(env ${3:+SIFT_CI_SHARD=$3} SIFT_CI_PG_MAJOR="${2/arm/18}" FLAVOUR="$2" "$0" --plan "$1") || return 1
			PLANS[$k]=$out
		fi
		PLAN_OUT=${PLANS[$k]}
	}
	if st_on flavors; then
		for pg in 16 17 18; do
			for m in quick; do
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
				(( n )) || { echo "FAIL: PostgreSQL $pg $m: the plan has no job that installs a flavor (the check is void)"; bad=1; }
			done
		done
	fi
	if st_on flavors; then
		# COMPAT_JOBS (affected.sh): what the compatibility entries of a push run is a plan with the regression batches, a crash test, a standby test, the isolation specs and the
		# upgrade test, on each major and architecture, and every name in it is a job
		for fl in 17 arm 19; do
			out=$(env SIFT_CI_JOBS="$COMPAT_JOBS" SIFT_CI_PG_MAJOR="${fl/arm/18}" FLAVOUR="$fl" "$0" --plan quick | awk '{ print $1 }') || { echo "FAIL: --plan quick with COMPAT_JOBS on $fl"; bad=1; continue; }
			for want in installcheck-batch1 isolation-1 tap-bm25/crash tap-bm25/standby tap-shell-compat/pg_upgrade/basic; do
				grep -qx "$want" <<<"$out" || { echo "FAIL: COMPAT_JOBS on $fl: the plan has no $want"; bad=1; }
			done
		done
	fi
	if st_on areas-files; then
		# scripts/areas.tsv: every tracked file is in a row, and every job of the plans (batches of short tests are cut from the tests of the areas) is in a group, so that
		# neither a file nor a test is left out of the selection; a core file selects everything
		mapfile -t tracked < <(git -C "$REPO" ls-files)
		mapfile -t unmatched < <(areas_unmatched "${tracked[@]}")
		miss=$(areas_invalid; (( ! ${#unmatched[@]} )) || areas_regex "${unmatched[@]}" 2>&1 >/dev/null)   # a file in no row is named by areas_regex
		[[ -z $miss ]] || { echo "FAIL: $miss"; bad=1; }
		# the table of rows that areas_invalid must flag (a path, a jobs expression) and the ones it must not (a group, a capture)
		t=$ST/areas.tsv
		printf '=g\tinstallcheck-a\nsrc/x/(.*)\t{g}|x\\1\nsrc/(\t.\nsrc/y\t(\nsrc/z\t-\nsrc/w\tfast:{g}\n' >"$t"
		areas_load "$t"
		[[ $(areas_invalid | grep -c 'invalid ERE') == 2 ]] || { echo "FAIL: areas_invalid does not flag exactly the two invalid rows: $(areas_invalid)"; bad=1; }
		printf 'src/x/.*\t.\nsrc/y\t-\n' >"$t"
		areas_load "$t"
		[[ $(areas_unmatched src/x/a src/y other) == other ]] || { echo "FAIL: areas_unmatched does not name the one file no row matches"; bad=1; }
		areas_load "$REPO/scripts/areas.tsv"
		grouped=$(printf '(%s)|' "${GROUP[@]}")
		for m in quick; do
			plan_of $m 18
			while read -r id _; do
				[[ $id == installcheck-batch* || $id =~ ${grouped%|} ]] || { echo "FAIL: areas: the $m job $id is in no group of scripts/areas.tsv"; bad=1; }
			done <<<"$PLAN_OUT"
		done
		for f in $(cd "$REPO/test/sql" && find . -name "*.sql" | sed "s|^\./||" | LC_ALL=C sort); do
			[[ installcheck-${f%.sql} =~ ${grouped%|} ]] || { echo "FAIL: areas: the test $f is in no group of scripts/areas.tsv"; bad=1; }
		done
	fi
	if st_on areas-select; then
		# a script of scripts/ selects every job that runs it, and a change of documentation, benchmarks or release notes selects none
		for m in quick; do
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
		[[ $(areas_regex docs/ANALYTICS.md CHANGELOG.md README.md bench/README.md) == '^$' ]] || { echo "FAIL: areas: a change of documentation or benchmarks selects jobs"; bad=1; }
		# the focus table (one file scripts/focus/NAME.tsv for each test, made by scripts/focus-gen.sh): a row for every regression test and none for a test that is gone; the lookup selects the
		# tests that execute a function, the tests of the area for a hot function or a change outside functions
		focus_rows=$(cat $(find "$REPO/scripts/focus" -name "*.tsv") | cut -f1 | grep -v '^=' | sort)
		focus_missing=$(comm -23 <((cd "$REPO/test/sql" && find . -name "*.sql" | sed "s|^\./||; s/\.sql$//") | sort) <(echo "$focus_rows"))
		[[ -z $focus_missing ]] || { echo "FAIL: focus: no scripts/focus/NAME.tsv for $(echo $focus_missing) (scripts/focus-gen.sh NAME...)"; bad=1; }
		focus_stale=$(comm -13 <((cd "$REPO/test/sql" && find . -name "*.sql" | sed "s|^\./||; s/\.sql$//") | sort) <(echo "$focus_rows"))
		[[ -z $focus_stale ]] || { echo "FAIL: focus: scripts/focus has files for $(echo $focus_stale), which are not tests"; bad=1; }
		read -r focus_t focus_tok _ < <(awk -F'\t' '$1 != "=hot" && $2 != "" { split($2, a, " "); print $1, a[1]; exit }' $(find "$REPO/scripts/focus" -name "*.tsv"))
		[[ -z $focus_t || installcheck-$focus_t =~ $(focus_regex "$focus_tok") ]] || { echo "FAIL: focus: $focus_tok does not select its test $focus_t"; bad=1; }
		focus_hot=$(awk -F'\t' '$1 == "=hot" { split($2, a, " "); print a[1]; exit }' "$REPO/scripts/focus/_hot.tsv")
		[[ -z $focus_hot || $(focus_regex "$focus_hot") == "$(focus_regex "${focus_hot%%:*}:*")" ]] || { echo "FAIL: focus: the hot function $focus_hot does not select the tests of its area"; bad=1; }
		# a function of a file split into a folder (scripts/moved-sources.tsv) selects the tests of the row made for the old file, with and without the prefix a split gave it
		read -r mv_new mv_old mv_pre < <(awk -F'\t' '!/^#/ && NF >= 2 { print $1, $2, $3; exit }' "$REPO/scripts/moved-sources.tsv")
		mv_fn=$(cat $(find "$REPO/scripts/focus" -name "*.tsv") | grep -o "$mv_old:[A-Za-z_0-9]*" | head -1 | cut -d: -f2)
		[[ -z $mv_fn || ( $(focus_regex "${mv_new}x.c:${mv_pre}${mv_fn}") == "$(focus_regex "$mv_old:$mv_fn")" && $(focus_regex "$mv_old:$mv_fn") != '^(coverage|isolation-emulated)$' ) ]] || { echo "FAIL: focus: a function of a file split into a folder does not select the tests of the row of the old file ($mv_old:$mv_fn)"; bad=1; }
		[[ $(focus_regex src/agg/accum.c:no_such_function) == '^(coverage|isolation-emulated)$' ]] || { echo "FAIL: focus: a function no row has selects tests"; bad=1; }
		# the assertions family of a pull request: the tests that run workers exist, and the file that starts them is a trigger
		n=0; for f in $(cd "$REPO/test/sql" && find . -name "*.sql" | sed "s|^\./||" | LC_ALL=C sort); do [[ installcheck-${f%.sql} =~ $ASSERT_JOBS ]] && n=$((n + 1)); done
		(( n >= 20 )) || { echo "FAIL: the assertions family ASSERT_JOBS selects $n regression tests, want 20 or more"; bad=1; }
		grep -qE "$ASSERT_MARK" "$REPO/src/agg/par/worker.c" || { echo "FAIL: src/agg/par/worker.c, which starts workers, is no trigger of the assertions family (ASSERT_MARK)"; bad=1; }
		# the fast tier: a source file runs the tests of its own area (src/agg: not the ones only the table area owns), a core file all of them
		fast_agg=$(areas_fast_regex src/agg/exec/run.c) fast_core=$(areas_fast_regex src/core/ops.c)
		plan_of quick 18
		while read -r id _; do
			[[ $id == installcheck-* ]] || continue
			[[ $id =~ $fast_core ]] || { echo "FAIL: areas: src/core/ops.c does not select the fast job $id"; bad=1; }
			[[ $id =~ ${GROUP[tbl]} && ! $id =~ ${GROUP[agg]} && ! $id =~ ${GROUP[col]} && $id =~ $fast_agg ]] && { echo "FAIL: areas: a src/agg file selects the table-only fast job $id"; bad=1; }
			[[ $id =~ ${GROUP[agg]} && ! $id =~ $fast_agg ]] && { echo "FAIL: areas: a src/agg file does not select the agg fast job $id"; bad=1; }
		done <<<"$PLAN_OUT"
		for f in src/core/ops.c "$REPO"/sql/*.sql "$REPO"/sql/[0-9]*/*.sql; do
			f=${f#"$REPO"/}
			[[ $(areas_regex "$f") == *'|.' ]] || { echo "FAIL: areas: $f does not select every job"; bad=1; }
		done
	fi
	if st_on budgets; then
		# the budget of a job: the slowest job of a hosted run is the floor of its shard, so a job over the ceiling is sliced or shrunk (CI_JOB_TIMEOUT kills it at 3 times the ceiling)
		over=$(awk -F'\t' -v max=330 '!/^#/ && ($2 > max || $3 > max) {print $1 " " $2 "/" $3}' "$REPO/scripts/weights-quick.tsv")
		[[ -z $over ]] || { echo "FAIL: scripts/weights-quick.tsv: a job takes more than 330 lane seconds (PostgreSQL 17/18): $over"; bad=1; }
	fi
	if st_on flavors; then
		# the smoke of the release library (release_library_tests): each name is a test the release build runs, or the quick tier would silently drop it
		plan_of quick 18
		prod=$(grep '^release-library-regress ' <<<"$PLAN_OUT")
		for t in $STOCK_SMOKE; do [[ " $prod " == *" $t "* ]] || { echo "FAIL: $t of STOCK_SMOKE is not in the release-library-regress jobs (no test of that name, a slow or test-only one)"; bad=1; }; done
		n=0; for f in $(cd "$REPO/test/sql" && find . -name "*.sql" | sed "s|^\./||" | LC_ALL=C sort); do [[ installcheck-${f%.sql} =~ $ASSERT_SKIP ]] && n=$((n + 1)); done
		(( n == 8 )) || { echo "FAIL: ASSERT_SKIP selects $n regression tests, want the 8 it names"; bad=1; }
	fi
	if st_on shards-quick; then
		# the quick tier of every flavour (PostgreSQL 17, 18, 19, arm64) is cut by weight: every job once, the shards within 15% of each other (the jobs after the pool counted in the last, times the lanes they idle), and each
		# shard queues its jobs heaviest first
		for fl in 17 18 19 arm; do
		for ns in 3 4; do
			plan_of quick $fl; all=$(awk '{print $1}' <<<"$PLAN_OUT" | sort)
			got= lo='' hi=0
			for (( k = 1; k <= ns; k++ )); do
				plan_of quick $fl $k/$ns; got+=$PLAN_OUT$'\n'; l=0 prev=0
				while read -r j _ where _; do
					weight_of "$j" $fl
					(( prev == 0 || WEIGHT <= prev )) || { echo "FAIL: shard $k/$ns of the quick tier on $fl queues $j ($WEIGHT s) after a lighter job ($prev s)"; bad=1; }
					prev=$WEIGHT
					[[ $where == after ]] && l=$(( l + WEIGHT * AFTER_LANES )) || l=$(( l + WEIGHT ))
				done <<<"$PLAN_OUT"
				(( l > hi )) && hi=$l
				[[ -z $lo ]] || (( l < lo )) && lo=$l
			done
			[[ $all == "$(awk 'NF {print $1}' <<<"$got" | sort)" ]] || { echo "FAIL: the $ns shards of the quick tier on $fl are not the plan, each job once"; bad=1; }
			(( hi * 100 <= lo * 115 )) || { echo "FAIL: the $ns shards of the quick tier on $fl are unbalanced by the weights (heaviest $hi s, lightest $lo s)"; bad=1; }
		done
		done
	fi
	if st_on tooling; then
		# the footprint sampler ends on the stop file and on TERM, prints its report and nothing on stderr (a hosted run printed "trap: line 2: unexpected EOF" and the sampler outlived the run)
		mkdir -p "$ST/stats/logs"
		for how in file term; do
			bash "$REPO/scripts/ci_stats.sh" "$ST/stats" >"$ST/stats/out" 2>"$ST/stats/err" &
			p=$!
			sleep 1.5
			if [[ $how == file ]]; then touch "$ST/stats/logs/stats.stop"; else kill "$p"; fi
			for _ in {1..10}; do kill -0 "$p" 2>/dev/null || break; sleep 0.5; done
			! kill -0 "$p" 2>/dev/null || { kill -9 "$p"; echo "FAIL: the footprint sampler does not end within 5 s of its $how"; bad=1; }
			wait "$p" 2>/dev/null
			{ grep -q 'peak total' "$ST/stats/out" && [[ ! -s $ST/stats/err ]]; } || { echo "FAIL: the footprint sampler ended by its $how without its report or with output on stderr: $(head -c 200 "$ST/stats/err")"; bad=1; }
			rm -f "$ST/stats/logs/stats.stop"
		done
	fi
	if st_on spread; then
		# the jobs that build older releases are all in the first shard (the workflow caches those builds for each shard), in every tier and layout
		for lay in quick:3 quick:4; do
			bad_pin=$(for k in 2 3; do plan_of "${lay%:*}" 18 $k/${lay#*:}; awk '$1 ~ /^tap-shell-(compat\/matrix|compat\/old_lib_refusal|table\/replica_mixed|compat\/pg_upgrade\/)/ {print $1}' <<<"$PLAN_OUT"; done)
			[[ -z $bad_pin ]] || { echo "FAIL: the ${lay%:*} tier cut in ${lay#*:} shards puts jobs that build older releases outside the first shard: $bad_pin"; bad=1; }
		done
	fi
	if st_on shards-quick; then
		# a failed check of a TAP shell test shows what the test had captured in $out
		out=$(SIFT_NODES=docker SIFT_TMP="$ST/tl" bash -c '. "$0/test/t/lib/common.bash"; out="line one"$'"'"'\n'"'"'"line two"; check "x" false; check "y" true' "$REPO" 2>&1)
		[[ $out == *"# what the check read (out):"*"# line two"*"ok 2 - y"* ]] || { echo "FAIL: a failed check of test/t/lib/common.bash does not show the out of the test: $out"; bad=1; }
	fi
	if st_on spread; then
		# the CPU-bound jobs (HEAVY_JOBS of scripts/ci/shards.sh) are spread: no shard has two more of them than another, in every tier and layout
		for lay in quick:3 quick:4; do
			counts=$(for k in $(seq 1 "${lay#*:}"); do plan_of "${lay%:*}" 18 $k/${lay#*:}; awk '$1 ~ /^(difftest|hardened|fuzz|release-library-regress|tap-shell-core\/scaling_slope)/ && $3 == "pool" {n++} END {print n + 0}' <<<"$PLAN_OUT"; done | sort -n)
			lo=$(head -1 <<<"$counts"); hi=$(tail -1 <<<"$counts")
			(( hi - lo <= 1 )) || { echo "FAIL: the ${lay%:*} tier cut in ${lay#*:} shards has $lo to $hi CPU-bound jobs in a shard (at most one apart)"; bad=1; }
		done
	fi
	if st_on shards-quick; then
		# the weights of scripts/ci-times.py are the median of the runs, not the newest (one hosted run is noisy)
		out=$(python3 -I "$REPO/scripts/ci-times.py" --selftest 2>&1) || { echo "FAIL: scripts/ci-times.py --selftest: $out"; bad=1; }
		# the plans of the shards made all at once (SIFT_CI_SHARD=all/N) are those of a run for the shard alone, for a shard of the pool and the last one (it runs the jobs after the pool)
		for k in 1/3 3/3; do
			m=quick
			plan_of $m 18 $k
			[[ $PLAN_OUT == "$(env SIFT_CI_SHARD=$k SIFT_CI_PG_MAJOR=18 FLAVOUR=18 "$0" --plan $m)" ]] || { echo "FAIL: the $m plan of shard $k made with all the shards differs from the plan of that shard alone"; bad=1; }
		done
	fi
	if st_on tooling; then
		# the fast gate runs on the release library, on Format 1 and under assertions; the Format 1.0 run queues neither the release library nor its canaries
		out=$(env SIFT_CI_PG_MAJOR=18 SIFT_PREBUILT_RELEASE=/stage "$0" --plan quick) || { echo "FAIL: --plan quick"; bad=1; }
		[[ $(awk '$1 == "fast-gate-release"' <<<"$out") == *"--format release --root /stage"* ]] || { echo "FAIL: the plan has no fast gate on the release library"; bad=1; }
		[[ $(awk '$1 == "compat-f1-fast-gate"' <<<"$out") == *"--format f1"* ]] || { echo "FAIL: the plan has no fast gate on Format 1"; bad=1; }
		[[ -z $(awk '$1 == "fast-gate-assert"' <<<"$out") ]] || { echo "FAIL: the plan queues the fast gate with assertions without the assertion shard"; bad=1; }
		[[ -z $(env SIFT_CI_PG_MAJOR=18 "$0" --plan quick | awk '$1 == "fast-gate-release"') ]] || { echo "FAIL: the plan queues the fast gate on the release library with no staged one"; bad=1; }
		[[ -z $(env SIFT_CI_PG_MAJOR=17 SIFT_PREBUILT_RELEASE=/stage "$0" --plan quick | awk '$1 ~ /^fast-gate/') ]] || { echo "FAIL: the plan on PostgreSQL 17 queues a fast gate (it needs extension_control_path)"; bad=1; }
		[[ $(env SIFT_CI_PG_MAJOR=18 SIFT_CI_ASSERT=1 "$0" --plan quick | awk '$1 == "fast-gate-assert"') == *"--format f2 --assert"* ]] || { echo "FAIL: the plan of the assertion shard has no fast gate with assertions"; bad=1; }
		[[ $(env SIFT_CI_PG_MAJOR=18 SIFT_CI_JOBS="$COMPAT_F1_JOBS" "$0" --plan quick | awk '{ printf "%s ", $1 }' | grep -o 'compat-f1-fast-gate' | wc -l) == 1 ]] || { echo "FAIL: the Format 1 lane (COMPAT_F1_JOBS) has not the fast gate once"; bad=1; }
		[[ -z $(env SIFT_FORMAT=1 SIFT_CI_PG_MAJOR=18 SIFT_PREBUILT_RELEASE=/stage "$0" --plan quick | awk '$1 ~ /^(fast-gate-(release|assert)|perf-gate-)/') ]] || { echo "FAIL: the Format 1.0 plan queues the fast gate on the release library or its canaries"; bad=1; }
		# a released extension script is never edited (an install upgraded in place would miss the change): new objects go into the next version's scripts
		(cd "$REPO" && scripts/sha256.sh --quiet -c scripts/released-sql.sha256) || { echo "FAIL: a released sql/pg_sift--*.sql differs from its tag; add the change to the scripts of the next version (a release appends its own scripts to scripts/released-sql.sha256)"; bad=1; }
		# scripts/leftovers.allow: a test that exists, an object and a reason on every line
		re='^([a-z0-9_]+): .+ # .+$'
		while IFS= read -r l; do
			[[ -z $l || $l == '#'* ]] && continue
			[[ $l =~ $re && -f $REPO/test/sql/${BASH_REMATCH[1]}.sql ]] || { echo "FAIL: scripts/leftovers.allow: \"$l\" is not \"test: regex # reason\" of an existing test"; bad=1; }
		done < "$REPO/scripts/leftovers.allow"
	fi
	if st_on portability; then
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
			'pg_sift\.so	the module of macOS is pg_sift.dylib: pg_sift$DLSUFFIX (test/t/lib/common.bash)'
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
	fi
	if st_on tooling; then
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
		for f in "docs/ANALYTICS.md false" "docs/ARCHITECTURE.md false" "src/agg/update/upd.c true" "bench/scaling/scaling.py true"; do   # a job the fast tier cannot run does not build it
			fp=$("$0" affected --github --files ${f% *} | grep '^fastpg=')
			[[ $fp == "fastpg=${f#* }" ]] || { echo "FAIL: affected --github for ${f% *} says '$fp', not fastpg=${f#* }"; bad=1; }
		done
		# the scaling gate is a job of the quick tier only for the files of the geometry (scripts/areas.tsv); a col change alone does not pick it
		[[ -z $(SIFT_CI_PG_MAJOR=18 "$0" --plan quick | grep scaling_slope) ]] || { echo "FAIL: tap-shell-core/scaling_slope is in the quick plan"; bad=1; }
		for f in "src/core/sift.h 1" "src/col/v1/scan/gorder.c 0"; do
			n=$("$0" affected --list --files ${f% *} | grep -c 'tap-shell-core/scaling_slope')
			[[ $n == "${f#* }" ]] || { echo "FAIL: affected for ${f% *} picks the scaling gate $n times, not ${f#* }"; bad=1; }
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
		# the scratch cap of the quick tier is the tmpfs budget of ci.sh in scripts/budgets.tsv: a shard that passes it fails
		cap=$(bash -c 'REPO=$1; set -- --plan quick; MODE=$1; usage() { :; }; . "$REPO/scripts/ci/config.sh"; echo "$MAX_MB"' _ "$REPO")
		want=$("$REPO/scripts/budget.sh" limits script:ci.sh | cut -d' ' -f2)
		[[ -n $want && $cap == "$want" ]] || { echo "FAIL: the scratch cap of the quick tier (CI_MAX_MB '$cap' in scripts/ci/config.sh) is not the tmpfs budget of script:ci.sh in scripts/budgets.tsv ('$want')"; bad=1; }
		# the scratch table (scripts/scratch.tsv): a row is a job of a plan and a number of MB, and scratch_table keeps the row of a job no summary has, replaces a measured one (plus 5%, to ten) and drops one
		# measured under 150 MB; a job that failed only raises its row
		known=$({ for pg in 17 18 19 arm; do plan_of quick $pg; echo "$PLAN_OUT"; done; env SIFT_CI_AFFECTED=. SIFT_CI_JOBS='fault/matrix|scaling_slope' SIFT_CI_PG_MAJOR=18 "$0" --plan quick; } | awk '{ print $1 }' | sort -u)
		stale=$(grep -v '^#' "$REPO/scripts/scratch.tsv" | cut -f1 | sort | comm -23 - <(echo "$known") | paste -sd' ' -)
		[[ -z $stale ]] || { echo "FAIL: scripts/scratch.tsv has rows for jobs that are in no plan: $stale (scripts/ci.sh --scratch FILE... updates the table)"; bad=1; }
		grep -v '^#' "$REPO/scripts/scratch.tsv" | grep -vE $'^[a-z0-9_/-]+\t[0-9]+$' >/dev/null && { echo "FAIL: scripts/scratch.tsv has a row that is not job, tab, MB"; bad=1; }
		printf '# header\nkept\t500\nreplaced\t900\nshrunk\t400\nraised\t300\nfailsmall\t300\n' >"$ST/table.tsv"
		printf 'replaced\tPASS\t5\t1000\t0\t\nshrunk\tPASS\t5\t120\t0\t\nnew\tPASS\t5\t200\t0\t\nsmall\tPASS\t5\t90\t0\t\nreplaced\tPASS\t5\t800\t0\t\nkept\tFAIL\t5\t10\t0\t\nraised\tFAIL\t5\t700\t0\t\nfailsmall\tFAIL\t5\t200\t0\t\nnewfail\tFAIL\t5\t400\t0\t\n' >"$ST/runs.tsv"
		[[ $(scratch_table "$ST/table.tsv" "$ST/runs.tsv" | paste -sd'|' -) == '# header|replaced	1050|raised	740|kept	500|newfail	420|failsmall	300|new	210' ]] ||
			{ echo "FAIL: scratch_table does not keep, replace and drop rows as measured: $(scratch_table "$ST/table.tsv" "$ST/runs.tsv" | paste -sd'|' -)"; bad=1; }
		# a regression batch that no run has measured counts BATCH_MB, not JOB_MB: the scheduler admits jobs by these estimates, and a batch peaks at several hundred MB
		got=$(bash -c 'REPO=$1; S=$REPO/scripts; set -- --plan quick; MODE=$1; usage() { :; }; for f in affected weights procs skips config jobs; do . "$REPO/scripts/ci/$f.sh"; done
			EST_MB=(); installcheck_batches agg_selftest_a agg_selftest_b >/dev/null; echo "${EST_MB[installcheck-batch1]:-} $BATCH_MB"' _ "$REPO")
		[[ -n ${got% *} && ${got% *} == "${got#* }" ]] || { echo "FAIL: an unmeasured regression batch does not count BATCH_MB in scripts/ci/jobs.sh ('$got')"; bad=1; }
		# the skips of a job: each form of skip line gives its reason, a line that is not a skip gives none, and a skip with no reason is named; the retained summary of a run lists the skips
		# of its jobs and fails when one has no reason. No test script says a skip without one
		t=$ST/skips.log
		printf '%s\n' 'ok 3 # skip needs PostgreSQL 18' '1..0 # SKIP no release tag' 'test/t/x.sh .. skipped: the seeds run in the full tier only' 'SKIP: server is not the ASan flavor' \
			'coldiff: SKIP: no data directory' 'ok 4 - fine' '# NOTICE:  extension "x" already exists, skipping' 'isa_check: ok, 13 objects skipped' >"$t"
		[[ $(job_skips "$t" | paste -sd'|' -) == 'needs PostgreSQL 18|no release tag|the seeds run in the full tier only|server is not the ASan flavor|no data directory' ]] ||
			{ echo "FAIL: job_skips gives '$(job_skips "$t" | paste -sd'|' -)' for the log of five skips and three lines that are none"; bad=1; }
		printf '%s\n' 'ok 5 # skip' '1..0 # SKIP' 'test/t/y.sh .. skipped' 'test/t/y.sh .. skipped: no reason given' 'SKIP' 'SKIP:' >"$t"
		[[ $(job_skips "$t" | grep -cxF "$NO_REASON") == 6 ]] || { echo "FAIL: job_skips does not name the six skips that give no reason: $(job_skips "$t" | paste -sd'|' -)"; bad=1; }
		printf '%s\n' 'ok 1 - fine' 'ok 2 # skip a reason' >"$t"
		[[ $(job_state PASS "$t") == PASS ]] || { echo "FAIL: a job that ran tests and skipped one is not PASS"; bad=1; }
		printf '%s\n' '1..0 # SKIP a reason' >"$t"
		[[ $(job_state PASS "$t") == SKIP && $(job_state FAIL "$t") == FAIL ]] || { echo "FAIL: a job that only skipped is not SKIP, or a failed one is not FAIL"; bad=1; }
		[[ $(job_row a PASS 3 40 "$t") == $'a\tSKIP\t3\t40\t1\ta reason' ]] || { echo "FAIL: job_row is not 'id state seconds peak skips reasons': $(job_row a PASS 3 40 "$t")"; bad=1; }
		printf 'a\tSKIP\t3\t40\t1\ta reason\nb\tPASS\t4\t50\t0\t\n' >"$ST/jobs.tsv"
		out=$(summary_report "$ST/jobs.tsv") || { echo "FAIL: summary_report fails for skips that all give a reason"; bad=1; }
		[[ $out == *'  SKIP a: a reason'* && $out == *'2 jobs: 1 pass, 0 fail, 1 skip; 1 report skips'* ]] || { echo "FAIL: summary_report does not list the skips and count the states: $out"; bad=1; }
		printf 'c\tPASS\t4\t50\t1\t%s\n' "$NO_REASON" >>"$ST/jobs.tsv"
		summary_report "$ST/jobs.tsv" >/dev/null && { echo "FAIL: summary_report passes a skip with no reason"; bad=1; }
		for f in $(git -C "$REPO" ls-files 'test/t/*.sh' 'test/t/*.bash'); do
			! grep -qE "(# SKIP|^[[:space:]]*skip [0-9]+)[[:space:]'\"]*(;|\$)" "$REPO/$f" || { echo "FAIL: $f says a skip and gives no reason"; bad=1; }
		done
		# -h and --help of every subcommand print the header and exit 0 before anything starts (the timeout is for a run that would start)
		TM=(); for tm in timeout gtimeout; do command -v $tm >/dev/null && { TM=($tm 10); break; }; done   # macOS has neither without coreutils
		for m in quick affected stop summary --times --plan --areas-regex; do for h in -h --help; do
			out=$(SIFT_CI_TMP=$ST/help "${TM[@]}" "$0" $m $h 2>&1)
			[[ $? == 0 && $out == '# Single entry point'* ]] || { echo "FAIL: ci.sh $m $h does not print the usage and exit 0"; bad=1; }
		done; done
		[[ ! -e $ST/help ]] || { echo "FAIL: ci.sh --help made a scratch directory"; bad=1; }
	fi
	if st_on formats; then
		# The Format 2.0 run (the default) and the Format 1.0 run (SIFT_FORMAT=1, scripts/ci/format1-skip.txt): the first holds the Format 2.0 tests, the proof of the coverage table and the release library; the second
		# runs on the same staged library everything else (the regression tests, the plan gates, the differential tests, the isolation, crash and standby tests, the fuzzers and the soaks), none of the
		# jobs the first covers, has no empty shard and reads no history of the tags. The compatibility entries and the shards of the first are balanced and read the history only where a job builds an older release
		has_job() { grep -qE "^$1(-[0-9]+)? " <<<"$2"; }
		for m in quick; do
			plan_of $m 18; f2=$PLAN_OUT
			f1=$(env SIFT_FORMAT=1 SIFT_CI_PG_MAJOR=18 FLAVOUR=18 "$0" --plan $m) || { echo "FAIL: formats: SIFT_FORMAT=1 --plan $m"; bad=1; continue; }
			want=(coverage hardened tap-col/f2/wal tap-shell-compat/pg_upgrade/basic tap-shell-compat/matrix release-library-regress)
			for j in "${want[@]}"; do has_job "$j" "$f2" || { echo "FAIL: formats: the Format 2.0 $m plan lacks $j"; bad=1; }; done
			for t in col/f2/am table/f2/parity; do grep -qE " $t( |\$)" <<<"$f2" || { echo "FAIL: formats: the Format 2.0 $m plan runs no regression test $t"; bad=1; }; done
			for j in coverage plan-gate plan-gate-tuned plan-gate-table plan-gate-table-tuned difftest-col difftest-col-tbl difftest-fuzz difftest-fuzz-tbl difftest-txn soak-col isolation tap-bm25/crash tap-bm25/standby tap-shell-bm25/store/crash; do
				has_job "$j" "$f1" || { echo "FAIL: formats: the Format 1.0 $m plan lacks $j"; bad=1; }
			done
			grep -qE '^installcheck-batch1 ' <<<"$f1" || { echo "FAIL: formats: the Format 1.0 $m plan has no regression batch"; bad=1; }
			for j in release-library-regress hardened isa-baseline tools-build format-isolation regress-batch difftest-col-selftest tap-col/f2/wal tap-col/f2/minimal_wal tap-compat/v51_full \
				tap-compat/v6_dump_restore_tablespace tap-shell-compat/pg_upgrade/basic tap-shell-compat/matrix tap-shell-compat/old_lib_refusal tap-shell-col/f2/standby; do
				! grep -q "^$j " <<<"$f1" || { echo "FAIL: formats: the Format 1.0 $m plan queues $j"; bad=1; }
			done
			! grep -qE ' (format2_|table_f2_|stor_f2_|f2_|compat_v1_)' <<<"$f1" || { echo "FAIL: formats: the Format 1.0 $m plan names a test of Format 2.0 or one that sets Format 1 by itself: $(grep -oE ' (format2_|table_f2_|stor_f2_|f2_|compat_v1_)[a-z0-9_]*' <<<"$f1" | sort -u | head -3 | tr '\n' ' ')"; bad=1; }
		done
		# shard_spread FORMAT FLAVOUR N [JOBS]: "LOWEST HIGHEST EMPTY" of the lane seconds of the N shards of the quick plan (the jobs after the pool weigh AFTER_LANES times), EMPTY the shards with no job
		shard_spread() {
			local out k j where l lo='' hi=0 empty=0 n
			out=$(env SIFT_FORMAT="$1" SIFT_CI_JOBS="${4:-}" SIFT_CI_PG_MAJOR="${2/arm/18}" FLAVOUR="$2" SIFT_CI_SHARD="all/$3" "$0" --plan quick) || return 1
			for (( k = 1; k <= $3; k++ )); do
				l=0 n=0
				while read -r _ j _ where _; do
					weight_of "$j" "$2"
					[[ $where == after ]] && l=$(( l + WEIGHT * AFTER_LANES )) || l=$(( l + WEIGHT ))
					n=$(( n + 1 ))
				done < <(awk -v k=$k '$1 == k' <<<"$out")
				(( n )) || empty=$(( empty + 1 ))
				(( l > hi )) && hi=$l
				[[ -z $lo ]] || (( l < lo )) && lo=$l
			done
			echo "$lo $hi $empty"
		}
		for spec in 2:18:4: 1:18:3: 2:17:2:$COMPAT_JOBS_17 2:19:2:$COMPAT_JOBS 2:arm:2:$COMPAT_JOBS_ARM; do
			IFS=: read -r fmt fl n re <<<"$spec"
			read -r lo hi empty < <(shard_spread "$fmt" "$fl" "$n" "$re") || { echo "FAIL: formats: --plan of Format $fmt on $fl in $n shards"; bad=1; continue; }
			(( empty == 0 )) || { echo "FAIL: formats: $empty of the $n shards of the quick plan of Format $fmt on $fl have no job"; bad=1; }
			(( hi * 100 <= lo * 115 )) || { echo "FAIL: formats: the $n shards of the quick plan of Format $fmt on $fl are unbalanced by the weights (heaviest $hi s, lightest $lo s)"; bad=1; }
		done
		# only the shard of PostgreSQL 18 on x86 that holds the jobs that build older releases reads the history of the tags
		history_of() { env SIFT_FORMAT="$1" SIFT_CI_JOBS="${5:-}" SIFT_CI_PG_MAJOR="${2/arm/18}" FLAVOUR="$2" SIFT_CI_SHARD="$3" "$0" --history "${4:-quick}"; }
		for spec in 17:"$COMPAT_JOBS_17" 19:"$COMPAT_JOBS" arm:"$COMPAT_JOBS_ARM"; do
			[[ $(history_of 2 "${spec%%:*}" 1/1 quick "${spec#*:}") == no ]] || { echo "FAIL: formats: the compatibility entry of ${spec%%:*} reads the history of the tags"; bad=1; }
		done
		for k in 1 2 3; do [[ $(history_of 1 18 $k/3) == no ]] || { echo "FAIL: formats: shard $k/3 of the Format 1.0 run reads the history of the tags"; bad=1; }; done
		[[ $(history_of 2 18 1/4) == yes ]] || { echo "FAIL: formats: shard 1/4 of the Format 2.0 run, which holds the jobs that build older releases, does not read the history of the tags"; bad=1; }
		for k in 2 3 4; do [[ $(history_of 2 18 $k/4) == no ]] || { echo "FAIL: formats: shard $k/4 of the Format 2.0 run reads the history of the tags"; bad=1; }; done
		out=$(SIFT_FORMAT=3 "$0" --plan quick 2>&1) && { echo "FAIL: formats: ci.sh accepts SIFT_FORMAT=3"; bad=1; }
		[[ $out == *'SIFT_FORMAT is 2'* ]] || { echo "FAIL: formats: ci.sh does not say which formats SIFT_FORMAT takes: $out"; bad=1; }
	fi
	# the process count is back to where it started: nothing a check began is left running
	leaked=$(for p in $(descendants $$); do [[ $p == "$BASHPID" || $before == *" $p "* ]] || ps -o pid=,args= -p "$p" 2>/dev/null; done)
	[[ -z $leaked ]] || { echo "FAIL: processes left running by the checks: $leaked"; bad=1; }
	(( bad )) || echo "plan-selftest (${ST_ONLY[*]:-all parts}): the plans of PostgreSQL 16, 17 and 18 install no flavor while the lanes run; every file and job is in scripts/areas.tsv; the shards, the portability of the scripts and the loud failures hold"
	exit $bad
fi
