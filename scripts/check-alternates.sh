#!/usr/bin/env bash
# pg_regress takes the first of X.out, X_1.out, X_2.out... that matches. A change of X.out that leaves an alternate X_N.out as it was fits one major of PostgreSQL and
# not the other, and the red shows only when that major runs. A change of an expected file therefore changes its alternates too, or a message of the commits says why not:
#   alt-unchanged: X reason
#   scripts/check-alternates.sh   the tree (an alternate NAME_N.out belongs to a test NAME that has NAME.out) and, when NEUTRAL_BASE is set (a landing sets it to the tip it
#                                 lands on), the commits of NEUTRAL_BASE..HEAD; the history before a landing is not judged, so the hosted lint checks the tree only
#   scripts/check-alternates.sh --selftest
# An alternate also keeps every hunk of the main output: the difference of NAME_N.out from NAME.out is made of version-specific hunks only, each listed in scripts/alternates.allow
# (path, sha1 of the hunk, the majors of PostgreSQL it was verified on, its first changed line). A change of the main output that the alternate does not follow shows as a
# hunk that is not listed, whatever the commit or the landing base.
#   scripts/check-alternates.sh --accept --on MAJORS NAME_N.out...   lists the hunks of those alternates as they are now, after each one was inspected as version-specific
#                                 and the alternate was probed on MAJORS (scripts/probe-alternates.sh prints them)
# A removed alternate is a claim that no major the CI runs needs it: the test passes on the main output on every one of them. A removal is recorded with all of the majors
# (17, 18, 18-arm64, 19), probed with scripts/probe-alternates.sh, and a landing refuses a deleted alternate that has no record:
#   scripts/check-alternates.sh --removed --on 17,18,18-arm64,19 NAME_N.out...
set -euo pipefail
ALLOW=${ALTERNATES_ALLOW:-scripts/alternates.allow}
MAJORS="17 18 18-arm64 19"

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

# listed KEY: the lines of the allow file whose first column is KEY
listed() { awk -F'\t' -v k="$1" '$1 == k' "$ALLOW" 2>/dev/null || true; }

# alternates: the alternate outputs of the tree that belong to a test with a main output
alternates() {
	local f n
	for f in $(git ls-files 'test/expected/*_[0-9]*.out'); do
		n=${f#test/expected/}; n=${n%.out}
		[[ $n =~ ^(.+)_[0-9]+$ && ! -f test/sql/$n.sql && -f test/expected/${BASH_REMATCH[1]}.out ]] && echo "$f" || true
	done
}

# echoed ALT...: a line for each alternate that drops a line of its test's SQL file from the main output: psql echoes that file on every version, so no listing excuses it
echoed() {
	python3 - "$@" <<'PY'
import collections, re, sys
for alt in sys.argv[1:]:
    main = re.sub(r'_[0-9]+\.out$', '.out', alt)
    sql = 'test/sql/' + main[len('test/expected/'):-4] + '.sql'
    src = {l.rstrip() for l in open(sql) if len(l.strip()) >= 12}
    echoes = lambda f: collections.Counter(l.rstrip() for l in open(f) if l.rstrip() in src)
    n = sum((echoes(main) - echoes(alt)).values())
    if n:
        print(f'{alt}: {n} lines of {sql} that {main} echoes are not in the alternate: no version changes them, so the alternate is stale: regenerate it from the main output (cp {main} {alt}) and inspect only the version-specific differences')
PY
}

# majors_ok VERSIONS: true when every one of the comma-separated VERSIONS is a major the CI runs
majors_ok() {
	local v
	[[ -n $1 ]] || return 1
	for v in ${1//,/ }; do [[ " $MAJORS " == *" $v "* ]] || return 1; done
}

# stale ALT...: a line for each hunk of the alternates that is not listed in the allow file, and for each listed hunk they no longer have
stale() {
	local f TMP; TMP=$(mktemp -d); trap "rm -rf $TMP" RETURN
	echoed "$@"
	for f in "$@"; do
		hunks "$f" | cut -f1,2 | sort -u >"$TMP/now"
		listed "$f" | cut -f1,2 | sort -u >"$TMP/listed"
		comm -23 "$TMP/now" "$TMP/listed" | cut -f2 | while read -r h; do
			echo "$f: a hunk that differs from ${f%_*}.out is not in $ALLOW ($(hunks "$f" | grep -F "$h" | cut -f3 | head -1)): regenerate the alternate from the main output (cp ${f%_*}.out $f) and inspect only the version-specific differences, then list them with scripts/check-alternates.sh --accept --on MAJORS $f"
		done
		comm -13 "$TMP/now" "$TMP/listed" | cut -f2 | while read -r h; do
			echo "$f: $ALLOW lists a hunk that the alternate no longer has ($(listed "$f" | grep -F "$h" | cut -f4 | head -1)): remove it with scripts/check-alternates.sh --accept --on MAJORS $f"
		done
	done
}

# records: a line for each entry of the allow file that names no major the CI runs, and each removal that was not probed on all of them or whose file is still in the tree
records() {
	local k f v rest
	[[ -f $ALLOW ]] || return 0
	while IFS=$'\t' read -r k f v rest; do
		if [[ $k == removed ]]; then
			[[ $(tr ', ' '\n\n' <<<"$v" | sort | xargs) == $(tr ' ' '\n' <<<"$MAJORS" | sort | xargs) ]] || echo "$f: removed on ${v:-no majors}, and a removal is probed on all of: ${MAJORS// /, } (scripts/probe-alternates.sh)"
			[[ -z $(git ls-files "$f") ]] || echo "$f: recorded as removed in $ALLOW, and it is in the tree"
		else
			majors_ok "$v" || echo "$k: the entry of hunk ${f:0:9} names the majors '$v', and they are some of: ${MAJORS// /, }"
		fi
	done <"$ALLOW"
}

# check BASE: a line for each breach, in the repository of the working directory; BASE empty checks the tree only
check() {
	local f n a
	for f in $(git ls-files 'test/expected/*_[0-9]*.out'); do
		n=${f#test/expected/}; n=${n%.out}
		[[ $n =~ ^(.+)_[0-9]+$ && ! -f test/sql/$n.sql ]] || continue
		[[ -f test/expected/${BASH_REMATCH[1]}.out ]] || echo "$f: an alternate, and test/expected/${BASH_REMATCH[1]}.out does not exist"
	done
	records
	[[ -n $1 ]] || return 0
	for f in $(git diff --name-only --no-renames --diff-filter=AM "$1" HEAD -- 'test/expected/*.out'); do
		n=${f#test/expected/}; n=${n%.out}
		git log --format=%B "$1..HEAD" | grep -E "^alt-unchanged: $n .+" >/dev/null && continue
		for a in $(git ls-files "test/expected/${n}_[0-9]*.out" | grep -E "^test/expected/${n}_[0-9]+[.]out$"); do
			git diff --quiet "$1" HEAD -- "$a" && echo "$f changed and $a did not: change the alternate, or say in a commit message \"alt-unchanged: $n <reason>\""
		done
	done
	for f in $(git diff --name-only --no-renames --diff-filter=D "$1" HEAD -- 'test/expected/*_[0-9]*.out'); do
		n=${f#test/expected/}; n=${n%_*}
		[[ -f test/expected/$n.out ]] || continue
		[[ -n $(listed removed | awk -F'\t' -v f="$f" '$2 == f') ]] || echo "$f was deleted and $ALLOW has no record of it: probe the test without the alternate on every major (scripts/probe-alternates.sh), then scripts/check-alternates.sh --removed --on ${MAJORS// /,} $f"
	done
}

# record_on MODE VERSIONS ALT...: the entries of the alternates (MODE accept: their hunks as they are now; removed: a removal) replace the ones in the allow file
record_on() {
	local mode=$1 on=$2 f tmp; shift 2; tmp=$(mktemp); mkdir -p "$(dirname "$ALLOW")"
	majors_ok "$on" || { echo "check-alternates: --on names the majors the alternates were probed on, some of: ${MAJORS// /, }"; return 1; }
	for f in "$@"; do
		{ awk -F'\t' -v f="$f" '$1 != f && !($1 == "removed" && $2 == f)' "$ALLOW" 2>/dev/null || true
		  if [[ $mode == accept ]]; then
			[[ -f $f ]] || { echo "check-alternates: $f is not a file" >&2; rm -f "$tmp"; return 1; }
			hunks "$f" | awk -F'\t' -v v="${on//,/ }" 'BEGIN { OFS = "\t" } { print $1, $2, v, $3 }'
		  else
			[[ ! -e $f ]] || { echo "check-alternates: $f is still a file: git rm it first" >&2; rm -f "$tmp"; return 1; }
			printf 'removed\t%s\t%s\t\n' "$f" "${on//,/ }"
		  fi; } >"$tmp"
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
	removal_selftest "$t" || return 1
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
	[[ $(h record_on accept "" test/expected/a_1.out 2>&1) == *"--on names the majors"* ]] || { echo "selftest: a listing without the majors was accepted"; return 1; }
	[[ $(h record_on accept 16 test/expected/a_1.out 2>&1) == *"--on names the majors"* ]] || { echo "selftest: a listing on a major the CI does not run was accepted"; return 1; }
	h record_on accept 17,19 test/expected/a_1.out
	[[ -z $(h stale test/expected/a_1.out) && -z $(h records) ]] || { echo "selftest: a listed version-specific hunk was refused"; return 1; }
	mkdir -p "$t/test/sql"; printf 'a line that psql echoes\none\n' >"$t/test/sql/a.sql"; printf 'a line that psql echoes\ntwo\n' >"$t/test/expected/a.out"
	printf 'a changed line that psql echoes\ntwo\n' >"$t/test/expected/a_1.out"; h record_on accept 17 test/expected/a_1.out
	[[ $(h stale test/expected/a_1.out) == *"the alternate is stale"* ]] || { echo "selftest: an alternate that drops an echoed line was accepted"; return 1; }
	printf 'one\ntwo\nthree\n4\n' >"$t/test/expected/a_1.out"; printf 'one\ntwo\nthree\nfour\n' >"$t/test/expected/a.out"; echo a >"$t/test/sql/a.sql"
	h record_on accept 17,19 test/expected/a_1.out
	printf 'ONE\ntwo\nthree\n4\n' >"$t/test/expected/a.out"; printf 'ONE\ntwo\nthree\n4\n' >"$t/test/expected/a_1.out"
	[[ $(h stale test/expected/a_1.out) == *"lists a hunk that the alternate no longer has"* ]] || { echo "selftest: a listed hunk that is gone was accepted"; return 1; }
}

# a removal: it needs a record on every major the CI runs, and a landing refuses a deleted alternate without one
removal_selftest() {
	local t=$1 base
	h() { (cd "$t" && "$@"); }
	printf 'one\ntwo\n' >"$t/test/expected/a.out"; cp "$t/test/expected/a.out" "$t/test/expected/a_1.out"; rm -f "$t/scripts/alternates.allow"
	g add -A; g commit -qm 'a and its alternate'; base=$(g rev-parse HEAD)
	g rm -q test/expected/a_1.out; g commit -qm 'remove the alternate'
	[[ $(h check "$base") == *"a_1.out was deleted and scripts/alternates.allow has no record of it"* ]] || { echo "selftest: a removal without a record was accepted"; return 1; }
	[[ $(h record_on removed 17,19 test/expected/a_1.out 2>&1; h records) == *"probed on all of"* ]] || { echo "selftest: a removal probed on two majors was accepted"; return 1; }
	h record_on removed 17,18,18-arm64,19 test/expected/a_1.out
	g add -A; g commit -qm 'record the removal'
	[[ -z $(h check "$base") ]] || { echo "selftest: a removal probed on every major was refused"; return 1; }
}

if [[ ${1:-} == --selftest ]]; then selftest; exit; fi
cd "$(git rev-parse --show-toplevel)"
if [[ ${1:-} == --accept || ${1:-} == --removed ]]; then
	mode=${1#--}; [[ $mode == accept ]] || mode=removed; shift
	[[ ${1:-} == --on && -n ${2:-} && $# -ge 3 ]] || { echo "usage: scripts/check-alternates.sh --accept|--removed --on MAJORS NAME_N.out..."; exit 2; }
	on=$2; shift 2; record_on "$mode" "$on" "$@"; exit
fi
base=
[[ -z ${NEUTRAL_BASE:-} ]] || base=$(git rev-parse --verify -q "$NEUTRAL_BASE^{commit}") || { echo "check-alternates: NEUTRAL_BASE=$NEUTRAL_BASE is not a commit"; exit 1; }
out=$(check "$base"; stale $(alternates))
[[ -z $out ]] || { echo "$out"; exit 1; }
echo "check-alternates: every alternate has its expected file${base:+; the changes since ${base:0:9} keep their alternates}"
