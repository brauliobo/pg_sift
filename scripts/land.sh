#!/usr/bin/env bash
# Land one pull request: merge the tip of its base into the branch, run the gates, push when needed, wait for the required checks, merge.
#
#   scripts/land.sh PR            land PR; landings of different PRs run side by side, a second run of the same PR exits 7 at once
#   scripts/land.sh --dry-run PR  the same without a push, a merge or a write on GitHub, and without a lock: prints the plan
#   scripts/land.sh --stats       per day the landings by exit code, and the median wall and CPU seconds by kind of PR (docs, src, scripts, mixed), from landings.tsv
#   scripts/land.sh --selftest    the locks, the exit codes, the cleanup and the privacy gate with fake work (no GitHub, no network)
#
# Exit codes: 0 merged (or already merged); 1 the required checks are red; 2 SOURCE CONFLICT (a conflict beyond the routine ones below: the owner merges the tip);
# 3 the merge commit failed; 4 a gate is red (lint, generated files, bench cases, privacy, focus table); 5 the PR deletes files (ALLOW_DELETE=1 after checking they are its
# own); 6 the PR is closed; 7 another run is landing this same PR; 75 try again (checks still pending or missing, the tip or the branch moved, GitHub unreachable): run it again.
#
# Steps: (1) when the PR head does not hold the tip, merge the tip into the branch. A conflict only in what both sides add to (CHANGELOG, bench/cases.tsv, bench/results/*.tsv,
# the list lines of Makefile, scripts/focus.tsv) or in a generated file is resolved; any other is exit 2. The CHANGELOG is
# checked after every merge (no marker left, no line of Unreleased twice, no entry moved out of Unreleased or lost): a broken one is exit 2 and nothing is pushed. If the tip
# moved while the gates ran, it is merged again (three rounds; the gates run once, the checks of the pushed head are the rest). (2) After a merge: scripts/lint.sh. (3) The privacy gate on
# the added lines, the commit messages and the PR title and body, and the focus-table row of every new test/sql file. (4) A push, always after a merge of the tip: GitHub
# squashes the PR diff onto the tip, and a branch that is behind it conflicts there even when the local merge was clean. (5) The required checks of the new head (the branch
# protection list, else neutral and lint) are waited for: up to CHECK_WAIT seconds (600) while pending, MISSING_WAIT (300) while they have not appeared. (6) The tip is fetched
# again; if it moved while the checks ran, the answer of GitHub on the PR is waited for (UNKNOWN lasts seconds after every push to the base): MERGEABLE merges, CONFLICTING merges the tip into the branch again and runs the gates of the new head (three attempts), anything else exit 75. (7) The merge, `gh pr merge --match-head-commit` of the commit that was
# checked: rebase, or squash (PR title as subject, PR body) when the branch holds a merge commit, which GitHub refuses to rebase. Never a force-push. The line in MERGED.md says
# `merge-up` and `changelog-conflict` when the landing needed them, so they can be counted.
#
# Two kernel locks, in this order. pr-N.lock, taken first and never waited for: a second run for the same PR exits 7 and touches nothing of the first (its work tree is its
# own mktemp directory, removed by that path alone). merge.lock, taken around the merge call alone: landings of different PRs prepare side by side and merge one at a time. The
# kernel drops a lock when its holder dies, however it ends, and the worker runs without the pr-N.lock descriptor, so nothing that outlives a landing keeps it. pr-N.info
# beside the lock (pid, start) is for people. TERM and INT stop the worker and remove the work tree. A KILL cannot clean up: the worker runs to its end. During a switch from a
# legacy driver, LAND_LEGACY_LOCK names its merge lock, taken after merge.lock around the same call while the file exists.
#
# State and settings, all outside git: LAND_DIR (default $XDG_STATE_HOME/pg_sift/land: the locks, the merged log MERGED.md, privacy.deny), LAND_DENY (the private deny list of the
# privacy gate: one extended regular expression per line, # comments; the gate fails closed without it), LAND_CLONE (the clone to fetch into, default this repository),
# LAND_REPO (OWNER/NAME, default the remote of the clone). The script is read whole before it runs, so a checkout of another version under a landing does not change it.
set -u
SELF=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")
HERE=${SELF%/*}
DIR=${LAND_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/pg_sift/land}
DENY=${LAND_DENY:-$DIR/privacy.deny}
CLONE=${LAND_CLONE:-$(git -C "$HERE" rev-parse --show-toplevel)}
dry=${LAND_DRY:-} child='' holding='' WORK='' PR='' CL=''

# ---- the privacy gate: private names do not enter the public repository (the list is private data and never committed)

# deny_patterns FILE: the usable lines of the deny list into FILE; 2 when the list is missing, empty or not a regular expression: the gate fails closed
deny_patterns() {
	[[ -r $DENY ]] && grep -v -e '^#' -e '^[[:space:]]*$' "$DENY" >"$1"
	[[ -s $1 ]] && { grep -q -i -E -f "$1" /dev/null; (( $? <= 1 )); } && return 0
	echo "land: no usable deny list at $DENY (LAND_DENY): refusing to merge unchecked" >&2
	return 2
}
# privacy_range BASE HEAD: 0 clean, 1 a hit (printed), 2 no deny list; the lines HEAD adds over BASE and the messages of its commits
privacy_range() {
	local pat rc=0
	pat=$(mktemp); deny_patterns "$pat" || { rm -f "$pat"; return 2; }
	{ git -C "$CLONE" diff -U0 "$1...$2" | grep -E '^(\+\+\+ |\+[^+])' | awk '/^\+\+\+ /{f=$2;next}{print f": "$0}'
	  git -C "$CLONE" log --format='%h %s%n%b' "$1..$2"; } | grep -i -E -f "$pat" | cut -c1-200 | grep . && rc=1
	rm -f "$pat"; return $rc
}
# privacy_text: the text on stdin (a PR title and body), same results
privacy_text() {
	local pat rc=0
	pat=$(mktemp); deny_patterns "$pat" || { rm -f "$pat"; return 2; }
	grep -i -E -f "$pat" | cut -c1-160 | grep . && rc=1
	rm -f "$pat"; return $rc
}

# ---- the worker

# merge_locked CMD...: CMD under merge.lock, then the lock of a legacy driver while its file exists (so an old and a new driver never merge at once): the one global lock,
# held for the merge call alone. `flock -o` closes the descriptor for CMD, which therefore cannot leak it; no lock is held while waiting for checks or sleeping.
merge_locked() {
	local w=${LAND_LOCK_WAIT:-3600}
	[[ -z ${LAND_LEGACY_LOCK:-} || ! -e $LAND_LEGACY_LOCK ]] || set -- flock -w "$w" -E 75 -o "$LAND_LEGACY_LOCK" "$@"
	flock -w "$w" -E 75 -o "$DIR/merge.lock" "$@"
}

# resolve_conflicts BASE MB HEAD: the routine conflicts of the merge in progress, by the rules above; 2 on any other
resolve_conflicts() {
	local f l
	while IFS= read -r f; do
		case $f in
			CHANGELOG.md|bench/cases.tsv|bench/results/*.tsv)
				sed -i '/^<<<<<<< /d;/^=======$/d;/^>>>>>>> /d' "$f"; git add "$f"; [[ $f != CHANGELOG.md ]] || CL=1 ;;
			Makefile|scripts/focus.tsv)
				# both sides only add list lines (TESTING_REGRESS += ..., focus rows): keep both once; anything else is the owner's
				if python3 "$HERE/land-union.py" "$f"; then git add "$f"; else echo "SOURCE CONFLICT in $f: owner must merge the tip"; git merge --abort; return 2; fi ;;
			README.md|bench/results/*|docs/OPTIONS.md|docs/LIMITS.md|docs/REFERENCE.md|docs/COL-TAM.md|docs/img/*|bench/*)
				git checkout -q --theirs -- "$f" 2>/dev/null; git add "$f" ;;
			*) echo "SOURCE CONFLICT in $f: owner must merge the tip"; git merge --abort; return 2 ;;
		esac
	done < <(git diff --name-only --diff-filter=U)
	return 0
}

# checks_verdict HEAD REQUIRED: ok | pending <names> | red <names> | missing <names>, for the checks of the commit HEAD, which must still be the PR head; of several runs of
# one check (a re-run) the latest decides
checks_verdict() {
	gh pr view "$pr" --repo "$repo" --json headRefOid,statusCheckRollup | jq -r --arg req "$2" --arg head "$1" '
		if .headRefOid != $head then "pending the new head of the PR" else
		.statusCheckRollup as $c | ($req | split(" ")) as $names
		| [ $names[] as $n | ([$c[] | select(.name == $n)]) as $r
			| ($r | sort_by(if .status == "COMPLETED" then .completedAt else "9999" end) | last) as $l
			| if ($r | length) == 0 then "missing \($n)"
			  elif $l.status != "COMPLETED" then "pending \($n)"
			  elif ($l.conclusion | IN("FAILURE","CANCELLED","TIMED_OUT","STARTUP_FAILURE","ACTION_REQUIRED","STALE")) then "red \($n)"
			  elif $l.conclusion != "SUCCESS" and $n == "neutral" then "red \($n) (no success)"
			  else empty end ]
		| if length == 0 then "ok" else (map(split(" ")[0]) | if index("red") then "red" elif index("pending") then "pending" else "missing" end) + " " + (map(split(" ")[1:] | join(" ")) | join(", ")) end end' || echo "pending GitHub unreachable"
}

# wait_checks HEAD REQUIRED: sets verdict; a pending check is waited for up to CHECK_WAIT seconds (600), one that has not appeared yet (GitHub starts the checks of a pushed
# head late) up to MISSING_WAIT (300), every LAND_POLL seconds (15); no lock is held
wait_checks() {
	local waited=0 limit poll=${LAND_POLL:-15}
	while :; do
		verdict=$(checks_verdict "$1" "$2")
		case $verdict in pending*) limit=${CHECK_WAIT:-600} ;; missing*) limit=${MISSING_WAIT:-300} ;; *) return 0 ;; esac
		(( waited < limit )) || return 0
		sleep "$poll" # wait: the required checks of this PR, and GitHub to show a pushed head
		waited=$((waited + poll))
	done
}

# fetch_tip: origin/BASE of the clone (bounded; runs side by side with other landings, so a held ref lock is waited for and a ref they just moved is fetched again)
fetch_tip() {
	local err
	for _ in 1 2 3; do
		err=$(timeout 120 git -C "$CLONE" -c core.filesRefLockTimeout=10000 fetch -q origin "+refs/heads/$base:refs/remotes/origin/$base" 2>&1) && return 0
		sleep 1 # wait: for the landing that just updated this ref (its fetch changed it between our read and our lock)
	done
	echo "$err" >&2
	return 1
}

# changelog_check: in the work tree, after a merge of the tip: the CHANGELOG.md must not be broken (see land-union.py); needs mb and before (the merge base, the commit merged into)
changelog_check() {
	git show "$mb:CHANGELOG.md" >"$WORK/cl.mb" && git show "$before:CHANGELOG.md" >"$WORK/cl.head" && git show "origin/$base:CHANGELOG.md" >"$WORK/cl.tip" || return 0
	python3 "$HERE/land-union.py" --changelog CHANGELOG.md "$WORK/cl.mb" "$WORK/cl.head" "$WORK/cl.tip" || { echo "SOURCE CONFLICT in CHANGELOG.md: the merge of the tip left it broken (above); nothing is pushed, owner must merge the tip"; return 2; }
}

# fetch_pr_head PR: prints the commit the head of PR points at. The ref that carries it is private to this run: runs of one PR overlap (a dry run takes no lock), and a ref
# they shared was deleted by one under the other
fetch_pr_head() {
	local h=refs/land/pr$1.$BASHPID sha
	timeout 120 git -C "$CLONE" fetch -q origin "+refs/pull/$1/head:$h" || return 1
	sha=$(git -C "$CLONE" rev-parse "$h"); git -C "$CLONE" update-ref -d "$h"
	echo "$sha"
}

# settle_mergeable PR: sets MG (MERGEABLE, CONFLICTING, UNKNOWN) and ST (mergeStateStatus) once GitHub has an answer; it answers UNKNOWN for a few seconds after every push to the
# base, so poll every LAND_POLL seconds (5), 24 times
settle_mergeable() {
	local i
	MG=UNKNOWN ST=
	for (( i = 0; i < 24; i++ )); do
		IFS=$'\t' read -r MG ST < <(gh pr view "$1" --repo "$repo" --json mergeable,mergeStateStatus -q '[.mergeable,.mergeStateStatus]|@tsv')
		[[ ${MG:-UNKNOWN} != UNKNOWN ]] && return 0
		sleep "${LAND_POLL:-5}" # wait: for GitHub to compute the mergeability of the PR on the moved tip
	done
	MG=UNKNOWN
}

# lint_slot CMD...: at most LAND_LINT_SLOTS (default 2) lints run on the box at once, each one low priority (a lint runs thirty checks side by side, and ten landings at the same time
# kept the CPU pressure at 40%); the others wait here with no lock but their own held. A tree that already passed lint on this box (same git tree, stamp in $DIR) is not linted again.
lint_slot() {
	local i n=${LAND_LINT_SLOTS:-2} rc tree stamp
	tree=$(git rev-parse HEAD^{tree}) stamp=$DIR/lint-pass-$tree
	[[ -f $stamp ]] && { echo "lint: the tree $tree passed before (cached)"; return 0; }
	while :; do
		for (( i = 1; i <= n; i++ )); do
			exec 8>"$DIR/lint-slot.$i"
			flock -n 8 || continue
			nice -n 19 "$@"; rc=$?
			exec 8>&-
			(( rc )) || { : >"$stamp"; find "$DIR" -name 'lint-pass-*' -mtime +3 -delete 2>/dev/null; }
			return $rc
		done
		sleep 3 # wait: for a slot
	done
}

# red MESSAGE [LOG]: a gate is red
red() { echo "$1"; [[ -z ${2:-} ]] || tail -n 3 "$2"; exit 4; }

worker() {
	pr=$1 repo=${LAND_REPO:-$(cd "$CLONE" && gh repo view --json nameWithOwner -q .nameWithOwner)}
	local docs_proved= meta_hits cand state base br head mb before round push=0 W=$WORK/wt dels prot required verdict meta prc tests rows missing merges how opts st sha ms f note=''
	[[ -n $repo ]] || { echo "cannot tell the GitHub repository (LAND_REPO)"; exit 75; }
	cd "$CLONE" || exit 1
	ROUNDS=0 T_DOCS=0 T_LINT=0 T_WAIT=0 KIND=- HOW=-
	trap 'dump_stats final' EXIT
	IFS=$'\t' read -r state base br have_ms < <(gh pr view "$pr" --repo "$repo" --json state,baseRefName,headRefName,milestone -q '[.state,.baseRefName,.headRefName,.milestone.title // ""]|@tsv') && [[ -n $br ]] || { echo "cannot read #$pr from GitHub"; exit 75; }
	case $state in MERGED) echo "#$pr is already merged"; exit 0 ;; CLOSED) echo "REFUSED #$pr: the PR is closed"; exit 6 ;; esac
	dels=$(gh pr view "$pr" --repo "$repo" --json files -q '[.files[]|select(.changeType=="DELETED")|.path]|join(" ")') || exit 75
	[[ -z $dels || -n ${ALLOW_DELETE:-} ]] || { echo "REFUSED #$pr: the PR deletes files ($dels); check they are its own, then ALLOW_DELETE=1"; exit 5; }
	fetch_tip && head=$(fetch_pr_head "$pr") || { echo "cannot fetch #$pr or $base"; exit 75; }

	cand=$head   # the commit the gates look at; the one that is checked and merged is the PR head, or what this run pushes
	for round in 1 2 3; do
		git merge-base --is-ancestor "origin/$base" "$cand" && break
		# the PR does not hold the tip, or the tip moved while the gates ran: merge it in, in a work tree of this run, and push the result. GitHub squashes the PR diff onto
		# the tip, and a branch that is behind it conflicts there even when the local merge was clean (every PR adds its CHANGELOG entry at the same spot)
		push=1
		[[ -d $W ]] || git worktree add -q --detach "$W" "$cand" || exit 1
		cd "$W" || exit 1
		before=$(git rev-parse HEAD); mb=$(git merge-base "origin/$base" "$before")
		if ! git merge --no-ff -q -m "merge $base into $br" "origin/$base" >/dev/null 2>&1; then
			resolve_conflicts "$base" "$mb" "$before" || exit $?
			git add -A
			git commit -q --no-edit || { echo "commit failed"; exit 3; }
		fi
		changelog_check || exit $?
		docs_proved= ROUNDS=$round t_=$SECONDS
		T_DOCS=$(( T_DOCS + SECONDS - t_ ))
		if ! git diff --quiet; then git add -A; git commit -q -m "docs: regenerate after the merge of the tip"; fi
		echo "round $round, src patch-id: PR $(git diff "$mb" "$before" -- src | git patch-id --stable | cut -c1-12), merged-up $(git diff "origin/$base" HEAD -- src | git patch-id --stable | cut -c1-12)"
		(( round == 1 )) || { cand=$(git rev-parse HEAD); cd "$CLONE" || exit 1; fetch_tip || { echo "cannot fetch $base"; exit 75; }; continue; }   # again: the checks of the pushed head are the gates
		t_=$SECONDS
		lint_slot scripts/lint.sh >"$WORK/lint.log" 2>&1 || { cp "$WORK/lint.log" "$DIR/lint-$pr.log"; echo "LINT RED (log: $DIR/lint-$pr.log)"; grep -E '^(FAIL|[A-Za-z-]+: .*(not|missing|differ))' "$WORK/lint.log" | head -n 5 | cut -c1-200; exit 4; }
		T_LINT=$(( T_LINT + SECONDS - t_ ))
		cand=$(git rev-parse HEAD); cd "$CLONE" || exit 1
		fetch_tip || { echo "cannot fetch $base"; exit 75; }
	done
	(( push )) || echo "up to date"

	# privacy and focus table on the commit to merge: before any push
	meta=$(gh pr view "$pr" --repo "$repo" --json title,body -q '.title + "\n" + .body') || { echo "cannot read the text of #$pr"; exit 75; }
	meta_hits=$(privacy_range "origin/$base" "$cand" 2>&1); prc=$?
	[[ $prc == 0 ]] || { head -n 5 <<<"$meta_hits"; red "privacy red (rc $prc)"; }
	privacy_text <<<"$meta" >/dev/null 2>&1 || red "privacy red in the title or body of #$pr"
	if git cat-file -e "$cand:scripts/focus.tsv" 2>/dev/null; then
		tests=$(git diff --name-only --diff-filter=A "$(git merge-base "origin/$base" "$cand")" "$cand" -- test/sql | sed 's#.*/##; s/\.sql$//' | sort)
		rows=$(git show "$cand:scripts/focus.tsv" | cut -f1 | grep -v '^[#=]' | sort)
		missing=$(comm -23 <(echo "$tests" | grep .) <(echo "$rows") | tr '\n' ' ')
		[[ -z $missing ]] || red "no row in scripts/focus.tsv for: $missing(run scripts/focus-gen.sh NAME... and commit scripts/focus.tsv)"
	fi

	required=
	if prot=$(gh api "repos/$repo/branches/$base/protection/required_status_checks" --jq '.contexts[]' 2>/dev/null); then required=$(sort -u <<<"$prot" | tr '\n' ' '); required=${required% }; fi
	required=${required:-neutral lint}
	if [[ -n $dry ]]; then
		if (( push )); then echo "dry run: would push the merge-up to $br, then wait for the required checks ($required)"; else echo "dry run: required checks ($required): $(checks_verdict "$head" "$required")"; fi
		merges=$(gh api "repos/$repo/pulls/$pr/commits" --paginate --jq '[.[] | select(.parents | length > 1)] | length' | awk '{ n += $1 } END { print n + 0 }')
		echo "dry run: would merge by $( ((merges > 0)) && echo squash || echo rebase) ($merges merge commit(s) in the PR range)"; exit 0
	fi
	if (( push )); then
		git push -q origin "$cand:refs/heads/$br" || { echo "push of $br rejected: the branch moved"; exit 75; }
		head=$cand
	fi
	ms=${base#integration/}; ms=${ms%%-*}  # a PR without a milestone gets the one of its integration branch (.github/workflows/milestone.yml does it on opening)
	if [[ -z $have_ms && $base == integration/* && $ms =~ ^[0-9]+\.[0-9]+$ ]]; then gh pr edit "$pr" --repo "$repo" --milestone "$ms" >/dev/null && echo "milestone $ms set on #$pr"; fi

	KIND=$(kind_of "origin/$base" "$head")
	t_=$SECONDS
	wait_checks "$head" "$required"
	T_WAIT=$(( T_WAIT + SECONDS - t_ ))
	echo "required checks ($required): $verdict"
	if [[ $verdict != ok ]]; then
		echo "REFUSED #$pr: required checks not green ($verdict)"
		case $verdict in red*) exit 1 ;; *) exit 75 ;; esac
	fi

	# the tip may have moved while the checks ran (docs-only PRs landed a minute apart do it every time): the branch is not protected "up to date", so GitHub merges the PR
	# onto the new tip when it can. GitHub answers UNKNOWN for a few seconds after every push to the base (that was a red rc=75 for a PR with nothing in common with the
	# tip): wait for the answer. A conflict is a new merge of the tip into the branch, and the gates of the new head run again from the top (most are cached by the content of
	# their inputs); three attempts, then rc=75.
	fetch_tip || { echo "cannot fetch $base"; exit 75; }
	if ! git merge-base --is-ancestor "origin/$base" "$head"; then
		settle_mergeable "$pr"
		case $MG in
			MERGEABLE) [[ $ST != BLOCKED && $ST != DRAFT ]] || { echo "the tip moved while the checks ran and GitHub calls #$pr $st: run land.sh again"; exit 75; } ;;
			CONFLICTING)
				if (( ${LAND_ATTEMPT:-1} < 3 )); then
					echo "the tip moved while the checks ran and conflicts with #$pr: merging it again (attempt $(( ${LAND_ATTEMPT:-1} + 1 )) of 3)"
					dump_stats retry
					git -C "$CLONE" worktree remove --force "$W" 2>/dev/null
					LAND_ATTEMPT=$(( ${LAND_ATTEMPT:-1} + 1 )) exec "${LAND_WORKER:-$SELF}" "$pr"
				fi
				echo "the tip moved while the checks ran and conflicts with #$pr three times: run land.sh again"; exit 75 ;;
			*) echo "the tip moved while the checks ran and GitHub does not say if #$pr merges ($MG) after two minutes: run land.sh again"; exit 75 ;;
		esac
	fi

	merges=$(gh api "repos/$repo/pulls/$pr/commits" --paginate --jq '[.[] | select(.parents | length > 1)] | length' | awk '{ n += $1 } END { print n + 0 }')
	if (( merges > 0 )); then
		how=squash; opts=(--squash --subject "$(gh pr view "$pr" --repo "$repo" --json title -q .title)" --body "$(gh pr view "$pr" --repo "$repo" --json body -q .body)")
	else
		how=rebase opts=(--rebase)
	fi
	HOW=$how
	echo "merge mode: $how ($merges merge commit(s) in the PR range)"
	for f in 1 2; do
		merge_locked gh pr merge "$pr" --repo "$repo" --match-head-commit "$head" "${opts[@]}" --delete-branch >/dev/null 2>"$WORK/merge.err"
		st=$(gh pr view "$pr" --repo "$repo" --json state -q .state)
		[[ $st == MERGED ]] && break
		echo "merge attempt $f state=$st: $(head -c 200 "$WORK/merge.err")"
		sleep 20 # wait: GitHub to settle after a refused merge
	done
	[[ $st == MERGED ]] || { echo "NOT MERGED #$pr (state $st)"; exit 75; }
	sha=$(gh pr view "$pr" --repo "$repo" --json mergeCommit -q .mergeCommit.oid)
	(( push )) && note+=" merge-up"; [[ -z ${CL:-} ]] || note+=" changelog-conflict"   # counted from the log: how many landings needed a new merge of the tip
	echo "#$pr $sha $(date -u +%H:%MZ) -> $base$note" >> "${LAND_LOG:-$DIR/MERGED.md}"
	echo "merged #$pr $sha -> $base ($how)"
}

# ---- landings.tsv: one line per landing (the holder writes it), so the cost of a landing is a median of real ones
# dump_stats WHY: what the worker measured so far, appended to $WORK/stats (a retry execs a new worker, the holder adds the attempts up)
dump_stats() {
	[[ -n ${WORK:-} ]] || return 0
	printf 'attempts 1\nrounds %s\ndocs %s\nlint %s\nwait %s\nkind %s\nmode %s\n' "${ROUNDS:-0}" "${T_DOCS:-0}" "${T_LINT:-0}" "${T_WAIT:-0}" "${KIND:--}" "${HOW:--}" >>"$WORK/stats"
}
# kind_of BASE HEAD: docs (only documentation), src (only src/ and test/), scripts (anything of scripts/, .github/, Makefile), else mixed
kind_of() {
	local f k=''
	while IFS= read -r f; do
		case $f in
			scripts/*|.github/*|Makefile) echo scripts; return ;;
			docs/*|*.md|CHANGELOG*) [[ -z $k || $k == docs ]] && k=docs || k=mixed ;;
			src/*|test/*) [[ -z $k || $k == src ]] && k=src || k=mixed ;;
			*) k=mixed ;;
		esac
	done < <(git -C "$CLONE" diff --name-only "$(git -C "$CLONE" merge-base "$1" "$2")" "$2" 2>/dev/null)
	echo "${k:--}"
}
# record_landing PR RC T0 CPU0: the line of landings.tsv
record_landing() {
	local pr=$1 rc=$2 t0=$3 cpu=$4 att rounds docs lint wait kind mode
	read -r att rounds docs lint wait < <(awk '$1 == "attempts" { a += $2 } $1 == "rounds" { r = $2 } $1 == "docs" { d += $2 } $1 == "lint" { l += $2 } $1 == "wait" { w += $2 } END { print a + 0, r + 0, d + 0, l + 0, w + 0 }' "$WORK/stats" 2>/dev/null)
	kind=$(awk '$1 == "kind" { k = $2 } END { print k == "" ? "-" : k }' "$WORK/stats" 2>/dev/null); mode=$(awk '$1 == "mode" { m = $2 } END { print m == "" ? "-" : m }' "$WORK/stats" 2>/dev/null)
	[[ -s $DIR/landings.tsv ]] || printf '#utc\tpr\trc\twall_s\tcpu_s\tattempts\trounds\tdocs_s\tlint_s\tchecks_wait_s\tkind\tmode\n' >>"$DIR/landings.tsv"
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$pr" "$rc" "$(( $(date +%s) - t0 ))" "$cpu" "${att:-1}" "$rounds" "$docs" "$lint" "$wait" "$kind" "$mode" >>"$DIR/landings.tsv"
}
# children_cpu: user + system seconds of the children this shell has waited for (the worker and everything below it): cutime and cstime of the stat file of this shell, which a
# `times | awk` cannot read (a pipeline is a subshell and has no children) and a comma locale prints with a comma
children_cpu() { sed 's/^.*) //' "/proc/$$/stat" | awk -v hz="$(getconf CLK_TCK)" '{ printf "%.1f", ($14 + $15) / hz }'; }
# stats: per day, the landings by exit code, and the median wall and CPU by kind (the exit 0 ones)
stats() {
	local f=$DIR/landings.tsv
	[[ -s $f ]] || { echo "no landings recorded in $f"; return 0; }
	echo "per day: landings by exit code"
	awk -F'\t' '!/^#/ { d = substr($1, 1, 10); n[d, $3]++; days[d] = 1; rcs[$3] = 1 } END { for (d in days) { l = d; for (r in rcs) l = l sprintf("  rc%s=%d", r, n[d, r]); print l } }' "$f" | sort
	echo "median of the landings that merged (rc 0), by kind: n, wall s, CPU s"
	awk -F'\t' '!/^#/ && $3 == 0 { print $11 "\t" $4 "\t" $5 }' "$f" | sort -k1,1 | awk -F'\t' '
		function med(a, n,   i, j, t) { for (i = 2; i <= n; i++) { t = a[i]; for (j = i - 1; j >= 1 && a[j] > t; j--) a[j + 1] = a[j]; a[j + 1] = t } return n % 2 ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2 }
		{ k = $1; n[k]++; w[k, n[k]] = $2; c[k, n[k]] = $3; kinds[k] = 1 }
		END { for (k in kinds) { m = n[k]; for (i = 1; i <= m; i++) { ws[i] = w[k, i]; cs[i] = c[k, i] } printf "%-8s %3d  %6.0f  %6.1f\n", k, m, med(ws, m), med(cs, m) } }' | sort
}

# ---- the holder: the lock of this PR, a worker without it, the cleanup

# shellcheck disable=SC2329 # run by the trap
cleanup() {
	[[ -z $WORK ]] || { git -C "$CLONE" worktree remove --force "$WORK/wt" 2>/dev/null; rm -rf "$WORK"; }
	[[ -z $holding ]] || rm -f "$DIR/pr-$PR.info"
}

# shellcheck disable=SC2329
stop() {
	trap '' TERM INT HUP
	[[ -z $child ]] || { kill -TERM -- "-$child" 2>/dev/null || kill -TERM "$child" 2>/dev/null; wait "$child" 2>/dev/null; }
	exit "$1"
}

holder() {
	local pr=$1 rc
	[[ $pr =~ ^[0-9]+$ ]] || { echo "land: '$pr' is not a pull request number" >&2; exit 2; }
	PR=$pr
	mkdir -p "$DIR" || exit 1
	if [[ -z $dry ]]; then
		exec 9>"$DIR/pr-$pr.lock" || exit 1
		flock -n 9 || { echo "land: #$pr is already running (informational, from pr-$pr.info: $(tr '\n' ' ' <"$DIR/pr-$pr.info" 2>/dev/null)); not touching it"; exit 7; }
		holding=1
		printf 'pid=%s\nstarted=%s\n' "$$" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"$DIR/pr-$pr.info.$$" && mv -f "$DIR/pr-$pr.info.$$" "$DIR/pr-$pr.info"
	fi
	WORK=$(mktemp -d "${TMPDIR:-/tmp}/land.XXXXXX") || exit 1
	trap cleanup EXIT; trap 'stop 143' TERM; trap 'stop 130' INT; trap 'stop 129' HUP
	# own process group, no lock descriptor: `wait` returns at once for a trapped signal, and a process that outlives the landing cannot keep the lock
	local t0 cpu0; t0=$(date +%s) cpu0=$(children_cpu)
	LAND_RUN=1 LAND_WORK=$WORK LAND_DRY=$dry LAND_DIR=$DIR LAND_CLONE=$CLONE setsid "${LAND_WORKER:-$SELF}" "$pr" 9>&- & child=$!
	wait "$child"; rc=$?
	child=''
	[[ -n $dry ]] || record_landing "$pr" "$rc" "$t0" "$(awk -v a="$(children_cpu)" -v b="$cpu0" 'BEGIN { printf "%.1f", a - b }')"
	exit $rc
}

main() {
	case ${1:-} in
		--selftest) exec "$HERE/land-selftest.sh" ;;
		--dry-run) dry=1; shift ;;
		--stats) stats; exit 0 ;;
	esac
	(( $# == 1 )) || { sed -n '3,6p' "$SELF" | cut -c3-; exit 2; }
	if [[ -n ${LAND_RUN:-} ]]; then WORK=$LAND_WORK; worker "$1"; else holder "$1"; fi
}
# the selftest sources this file for the privacy functions and runs nothing
[[ ${BASH_SOURCE[0]} != "$0" ]] || { main "$@"; exit $?; }
