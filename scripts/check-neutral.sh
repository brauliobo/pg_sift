#!/usr/bin/env bash
# Neutral wording guard: no tracked file (its name or its text) and no commit subject of the branch contains a name of the denylist
# (scripts/neutral.denylist: one base64 word per line, matched case-insensitively as a fixed string; the words are product-specific names,
# never generic ones, and are kept encoded so that the repository does not contain them).
#
#   scripts/check-neutral.sh              the tracked files of the work tree and the subjects of origin/main..HEAD
#   scripts/check-neutral.sh --selftest   the guard flags every word and passes a neutral text and the repository
#
# Exit 1 with the file and lines (the word itself is not printed) or the commit of every hit.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
PAT=$(mktemp)
trap 'rm -f "$PAT"' EXIT
grep -v -e '^#' -e '^$' "$REPO/scripts/neutral.denylist" | while read -r w; do printf '%s' "$w" | base64 -d; echo; done >"$PAT"

# match: stdin holds a word of the denylist
match() { grep -q -i -F -f "$PAT"; }

check_repo() {
	local bad=0 f base sha subj hits
	cd "$REPO"
	while IFS= read -r f; do
		[[ $f == scripts/neutral.denylist ]] || ! match <<<"$f" || { echo "neutral: a file name holds a name of the denylist"; bad=1; }
	done < <(git ls-files)
	# the denylist itself is encoded; every other file is read as text (binary files are skipped)
	hits=$(git grep -I -i -n -F -f "$PAT" -- . ':(exclude)scripts/neutral.denylist' | cut -d: -f1,2 || true)
	[[ -z $hits ]] || { sed 's/^/neutral: a name of the denylist at /' <<<"$hits"; bad=1; }
	if base=$(git merge-base origin/main HEAD 2>/dev/null); then
		while IFS=' ' read -r sha subj; do
			if match <<<"$subj"; then echo "neutral: commit $sha has a subject that holds a name of the denylist"; bad=1; fi
		done < <(git log --format='%h %s' "$base..HEAD")
	fi
	return $bad
}

selftest() {
	local w bad=0 n=0
	while IFS= read -r w; do
		n=$((n + 1))
		match <<<"x ${w^^} y" || { echo "FAIL: the word $n of the denylist is not matched"; bad=1; }
	done <"$PAT"
	(( n >= 10 )) || { echo "FAIL: the denylist has $n words"; bad=1; }
	if match <<<"fact table, entities, entity_id, group_members, set filter, membership table, CREATE SUBSCRIPTION, code review"; then
		echo "FAIL: a neutral sample matches"; bad=1
	fi
	check_repo || bad=1
	(( bad )) || echo "neutral-selftest: $n words flagged, a neutral sample and the repository pass"
	return $bad
}

case ${1:-} in
	--selftest) selftest ;;
	"") check_repo && echo "neutral: ok" ;;
	*) sed -n '2,9p' "$0"; exit 1 ;;
esac
