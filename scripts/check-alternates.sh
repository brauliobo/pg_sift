#!/usr/bin/env bash
# pg_regress takes the first of X.out, X_1.out, X_2.out... that matches. A change of X.out that leaves an alternate X_N.out as it was fits one major of PostgreSQL and
# not the other, and the red shows only when that major runs. A change of an expected file therefore changes its alternates too, or a message of the commits says why not:
#   alt-unchanged: X reason
#   scripts/check-alternates.sh   the tree (an alternate NAME_N.out belongs to a test NAME that has NAME.out) and, when NEUTRAL_BASE is set (a landing sets it to the tip it
#                                 lands on), the commits of NEUTRAL_BASE..HEAD; the history before a landing is not judged, so the hosted lint checks the tree only
#   scripts/check-alternates.sh --selftest
set -euo pipefail

# check BASE: a line for each breach, in the repository of the working directory; BASE empty checks the tree only
check() {
	local f n a
	for f in $(git ls-files 'test/expected/*_[0-9]*.out'); do
		n=${f##*/}; n=${n%.out}
		[[ $n =~ ^(.+)_[0-9]+$ && ! -f test/sql/$n.sql ]] || continue
		[[ -f test/expected/${BASH_REMATCH[1]}.out ]] || echo "$f: an alternate, and test/expected/${BASH_REMATCH[1]}.out does not exist"
	done
	[[ -n $1 ]] || return 0
	for f in $(git diff --name-only --no-renames --diff-filter=AM "$1" HEAD -- 'test/expected/*.out'); do
		n=${f##*/}; n=${n%.out}
		git log --format=%B "$1..HEAD" | grep -qE "^alt-unchanged: $n .+" && continue
		for a in $(git ls-files "test/expected/${n}_[0-9]*.out" | grep -E "^test/expected/${n}_[0-9]+[.]out$"); do
			git diff --quiet "$1" HEAD -- "$a" && echo "$f changed and $a did not: change the alternate, or say in a commit message \"alt-unchanged: $n <reason>\""
		done
	done
}

selftest() {
	local t out; t=$(mktemp -d); trap "rm -rf $t" RETURN
	g() { git -C "$t" -c user.name=t -c user.email=t@example.com "$@"; }
	c() { (cd "$t" && check "$1"); }
	g init -q; mkdir -p "$t/test/sql" "$t/test/expected"
	for n in a b; do echo $n >"$t/test/sql/$n.sql"; echo $n >"$t/test/expected/$n.out"; done
	echo a >"$t/test/expected/a_1.out"; echo d >"$t/test/sql/d_1.sql"; echo d >"$t/test/expected/d_1.out"
	g add .; g commit -qm base
	[[ -z $(c '') ]] || { echo "selftest: a sound tree was refused"; return 1; }
	echo x >"$t/test/expected/a.out"; g commit -qam 'change a'
	[[ $(c HEAD~1) == *"a_1.out did not"* ]] || { echo "selftest: a changed expected file with an unchanged alternate was accepted"; return 1; }
	g commit -q --amend -m 'change a' -m 'alt-unchanged: a the wording is the same'
	[[ -z $(c HEAD~1) ]] || { echo "selftest: the alt-unchanged line was not taken"; return 1; }
	echo x >"$t/test/expected/a_1.out"; echo x >"$t/test/expected/b.out"; g commit -qam 'change both'
	[[ -z $(c HEAD~1) ]] || { echo "selftest: a changed alternate, or a file without one, was refused"; return 1; }
	echo y >"$t/test/expected/c_1.out"; g add .; g commit -qm orphan
	[[ $(c '') == *"c_1.out: an alternate"* ]] || { echo "selftest: an alternate without its expected file was accepted"; return 1; }
	echo 'check-alternates selftest: ok'
}

[[ ${1:-} != --selftest ]] || { selftest; exit; }
cd "$(git rev-parse --show-toplevel)"
base=
[[ -z ${NEUTRAL_BASE:-} ]] || base=$(git rev-parse --verify -q "$NEUTRAL_BASE^{commit}") || { echo "check-alternates: NEUTRAL_BASE=$NEUTRAL_BASE is not a commit"; exit 1; }
out=$(check "$base")
[[ -z $out ]] || { echo "$out"; exit 1; }
echo "check-alternates: every alternate has its expected file${base:+; the changes since ${base:0:9} keep their alternates}"
