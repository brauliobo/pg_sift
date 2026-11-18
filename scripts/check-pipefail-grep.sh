#!/usr/bin/env bash
# A script under pipefail must not pipe into `grep -q`: grep exits on its first match, the producer that still writes dies of SIGPIPE, and the pipeline fails with 141 whether or not
# the pattern matched. It passes while the producer's output fits one pipe write and fails when the host is loaded enough to separate the writes (bench/test/test_resume.sh: "the
# orphan of the killed run is not found by its environment"). Read the output into a variable ([[ $(cmd) == *x* ]]) or let grep read all of it (grep x >/dev/null).
#
#   scripts/check-pipefail-grep.sh             the shell files of the tree that set pipefail, and the parts scripts/ci.sh sources
#   scripts/check-pipefail-grep.sh --selftest
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail

PIPE_Q='(^|[^|])\|[[:space:]]*grep[[:space:]]+(-[a-zA-Z]*q|--quiet)'

violations() { # FILE...: the lines that pipe into grep -q, in the files that set pipefail or are sourced by ci.sh (which does)
	local f
	for f in "$@"; do
		grep -qE 'pipefail' "$f" || [[ $f == scripts/ci/* ]] || continue
		grep -nE "$PIPE_Q" "$f" | sed "s#^#$f:#" || true
	done
}

if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
	g='| grep'   # the sample lines are built, so that this file does not pipe into grep -q itself
	printf 'set -o pipefail\nprod %s -q x || true\nprod %s -qE y\nprod %s x >/dev/null\n[[ $(prod) == *x* ]] || prod |%s -q y\n' "$g" "$g" "$g" "$g" >"$d/under.sh"
	printf 'prod %s -q x\n' "$g" >"$d/plain.sh"
	got=$(violations "$d/under.sh" "$d/plain.sh")
	[[ $got == "$d/under.sh:2:prod $g -q x || true"$'\n'"$d/under.sh:3:prod $g -qE y" ]] || { echo "check-pipefail-grep: selftest: unexpected verdict: $got"; exit 1; }
	echo "check-pipefail-grep: selftest ok"
	exit 0
fi

cd "$(dirname "$0")/.."
mapfile -t files < <(git ls-files 'scripts/*.sh' 'scripts/ci/*.sh' 'test/t/*.sh' 'test/t/*.bash' 'bench/*.sh' 'bench/*/*.sh' 'packaging/*.sh' bench/bench)
mapfile -t hits < <(violations "${files[@]}")
(( ${#hits[@]} == 0 )) || {
	printf '%s\n' "${hits[@]}"
	echo "check-pipefail-grep: ${#hits[@]} line(s) pipe into grep -q under pipefail: the producer dies of SIGPIPE when grep exits on the first match and the pipeline fails under load; read the output into a variable or drop -q and send grep to /dev/null"
	exit 1
}
echo "check-pipefail-grep: ok, no script under pipefail pipes into grep -q (${#files[@]} files)"
