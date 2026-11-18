#!/usr/bin/env bash
# pg_regress takes the first of X.out, X_1.out, X_2.out... that matches. A change of X.out that leaves an alternate X_N.out as it was fits one major of PostgreSQL and
# not the other, and the red shows only when that major runs. A change of an expected file therefore changes its alternates too, or a message of the commits says why not:
#   alt-unchanged: X reason
#   scripts/check-alternates.sh   the tree (an alternate NAME_N.out belongs to a test NAME that has NAME.out) and, when NEUTRAL_BASE is set (a landing sets it to the tip it
#                                 lands on), the commits of NEUTRAL_BASE..HEAD; the history before a landing is not judged, so the hosted lint checks the tree only
#   scripts/check-alternates.sh --selftest
# An alternate also keeps every hunk of the main output: the difference of NAME_N.out from NAME.out is made of version-specific hunks only, each listed in scripts/alternates.allow
# (path, sha1 of the hunk, its first changed line). A change of the main output that the alternate does not follow shows as a hunk that is not listed, whatever the commit or the landing base.
#   scripts/check-alternates.sh --accept NAME_N.out...   lists the hunks of those alternates as they are now, after each one was inspected as version-specific
set -euo pipefail
ALLOW=${ALTERNATES_ALLOW:-scripts/alternates.allow}

# hunks ALT...: "ALT<TAB>sha1<TAB>first changed line" for each hunk of the difference of each alternate from its main output
hunks() {
	python3 - "$@" <<'PY'
import hashlib, re, subprocess, sys
for alt in sys.argv[1:]:
    main = re.sub(r'_[0-9]+\.out$', '.out', alt)
    out = subprocess.run(['diff', '-U0', main, alt], capture_output=True, text=True).stdout.split('\n')[2:]
    hunk = []
    for line in out + ['@@']:
        if line.startswith('@@'):
            if hunk:
                print(alt, hashlib.sha1('\n'.join(hunk).encode()).hexdigest(), hunk[0][1:81].strip(), sep='\t')
            hunk = []
        elif line:
            hunk.append(line)
PY
}

# alternates: the alternate outputs of the tree that belong to a test with a main output
alternates() {
	local f n
	for f in $(git ls-files 'test/expected/*_[0-9]*.out'); do
		n=${f##*/}; n=${n%.out}
		[[ $n =~ ^(.+)_[0-9]+$ && ! -f test/sql/$n.sql && -f test/expected/${BASH_REMATCH[1]}.out ]] && echo "$f" || true
	done
}

# echoed ALT...: a line for each alternate that drops a line of its test's SQL file from the main output: psql echoes that file on every version, so no listing excuses it
echoed() {
	python3 - "$@" <<'PY'
import re, subprocess, sys
for alt in sys.argv[1:]:
    main = re.sub(r'_[0-9]+\.out$', '.out', alt)
    sql = 'test/sql/' + main.rsplit('/', 1)[1][:-4] + '.sql'
    src = {l.rstrip() for l in open(sql) if len(l.strip()) >= 12}
    out = subprocess.run(['diff', '-U0', main, alt], capture_output=True, text=True).stdout.split('\n')[2:]
    n = sum(1 for l in out if l.startswith('-') and l[1:].rstrip() in src)
    if n:
        print(f'{alt}: {n} lines of {sql} that {main} echoes are not in the alternate: no version changes them, so the alternate is stale: regenerate it from the main output (cp {main} {alt}) and inspect only the version-specific differences')
PY
}

# stale ALT...: a line for each hunk of the alternates that is not listed in the allow file, and for each listed hunk they no longer have
stale() {
	local f TMP; TMP=$(mktemp -d); trap "rm -rf $TMP" RETURN
	echoed "$@"
	for f in "$@"; do
		hunks "$f" | cut -f1,2 | sort -u >"$TMP/now"
		{ grep -F "$f	" "$ALLOW" 2>/dev/null || true; } | cut -f1,2 | sort -u >"$TMP/listed"
		comm -23 "$TMP/now" "$TMP/listed" | cut -f2 | while read -r h; do
			echo "$f: a hunk that differs from ${f%_*}.out is not in $ALLOW ($(hunks "$f" | grep -F "$h" | cut -f3 | head -1)): regenerate the alternate from the main output (cp ${f%_*}.out $f) and inspect only the version-specific differences, then list them with scripts/check-alternates.sh --accept $f"
		done
		comm -13 "$TMP/now" "$TMP/listed" | cut -f2 | while read -r h; do
			echo "$f: $ALLOW lists a hunk that the alternate no longer has ($(grep -F "$h" "$ALLOW" | cut -f3 | head -1)): remove it with scripts/check-alternates.sh --accept $f"
		done
	done
}

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
		git log --format=%B "$1..HEAD" | grep -E "^alt-unchanged: $n .+" >/dev/null && continue
		for a in $(git ls-files "test/expected/${n}_[0-9]*.out" | grep -E "^test/expected/${n}_[0-9]+[.]out$"); do
			git diff --quiet "$1" HEAD -- "$a" && echo "$f changed and $a did not: change the alternate, or say in a commit message \"alt-unchanged: $n <reason>\""
		done
	done
}

# accept ALT...: the hunks of the alternates as they are now replace their lines of the allow file
accept() {
	local f tmp; tmp=$(mktemp); mkdir -p "$(dirname "$ALLOW")"
	for f in "$@"; do
		[[ -f $f ]] || { echo "check-alternates: $f is not a file"; rm -f "$tmp"; return 1; }
		{ grep -vF "$f	" "$ALLOW" 2>/dev/null || true; hunks "$f"; } >"$tmp"
		sort -u -o "$ALLOW" "$tmp"
	done
	rm -f "$tmp"
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
	g rm -q --cached -r test/expected/c_1.out; rm "$t/test/expected/c_1.out"; g commit -qam 'drop the orphan'
	hunk_selftest "$t" || return 1
	echo 'check-alternates selftest: ok'
}

# the hunks of an alternate: one that follows a change of the main output is refused, a listed version-specific one is not, a listed one that is gone is refused
hunk_selftest() {
	local t=$1 n
	h() { (cd "$t" && "$@"); }
	printf 'one\ntwo\nthree\nfour\n' >"$t/test/expected/a.out"; cp "$t/test/expected/a.out" "$t/test/expected/a_1.out"
	g add .
	[[ -z $(h stale test/expected/a_1.out) ]] || { echo "selftest: an alternate equal to its main output was refused"; return 1; }
	printf 'one\nTWO\nthree\nfour\n' >"$t/test/expected/a.out"
	n=$(h stale test/expected/a_1.out)
	[[ $n == *"a_1.out: a hunk that differs from test/expected/a.out is not in"*"regenerate the alternate from the main output"*"version-specific"* ]] || { echo "selftest: a main output the alternate did not follow was accepted"; return 1; }
	printf 'one\ntwo\nthree\n4\n' >"$t/test/expected/a_1.out"; printf 'one\ntwo\nthree\nfour\n' >"$t/test/expected/a.out"
	[[ -n $(h stale test/expected/a_1.out) ]] || { echo "selftest: a version-specific hunk that is not listed was accepted"; return 1; }
	h accept test/expected/a_1.out
	mkdir -p "$t/test/sql"; printf 'a line that psql echoes\none\n' >"$t/test/sql/a.sql"; printf 'a line that psql echoes\ntwo\n' >"$t/test/expected/a.out"
	printf 'a changed line that psql echoes\ntwo\n' >"$t/test/expected/a_1.out"; h accept test/expected/a_1.out
	[[ $(h stale test/expected/a_1.out) == *"the alternate is stale"* ]] || { echo "selftest: an alternate that drops an echoed line was accepted"; return 1; }
	printf 'one\ntwo\nthree\n4\n' >"$t/test/expected/a_1.out"; printf 'one\ntwo\nthree\nfour\n' >"$t/test/expected/a.out"; rm "$t/test/sql/a.sql"; echo a >"$t/test/sql/a.sql"; h accept test/expected/a_1.out
	[[ -z $(h stale test/expected/a_1.out) ]] || { echo "selftest: a listed version-specific hunk was refused"; return 1; }
	printf 'ONE\ntwo\nthree\n4\n' >"$t/test/expected/a.out"; printf 'ONE\ntwo\nthree\n4\n' >"$t/test/expected/a_1.out"
	[[ $(h stale test/expected/a_1.out) == *"lists a hunk that the alternate no longer has"* ]] || { echo "selftest: a listed hunk that is gone was accepted"; return 1; }
}

[[ ${1:-} != --selftest ]] || { selftest; exit; }
cd "$(git rev-parse --show-toplevel)"
[[ ${1:-} != --accept ]] || { shift; accept "$@"; exit; }
base=
[[ -z ${NEUTRAL_BASE:-} ]] || base=$(git rev-parse --verify -q "$NEUTRAL_BASE^{commit}") || { echo "check-alternates: NEUTRAL_BASE=$NEUTRAL_BASE is not a commit"; exit 1; }
out=$(check "$base"; stale $(alternates))
[[ -z $out ]] || { echo "$out"; exit 1; }
echo "check-alternates: every alternate has its expected file${base:+; the changes since ${base:0:9} keep their alternates}"
