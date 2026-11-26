#!/usr/bin/env bash
# bash unsets NAME_PID as soon as it reaps the coprocess NAME (a psql that read \q, a server that crashed): a later `wait "$NAME_PID"` reads an unset variable, which a test under
# `set -u` (test/t/lib/common.bash) dies on, when the coprocess was quick to exit. A shell test copies the pid when it starts the coprocess (`holder=$NAME_PID`) and waits on the copy.
#   scripts/check-tap-coproc.sh [--selftest]
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

check() { # DIR: the lines of the shell tests that read the pid of one of their coprocesses other than to copy it
	local f name
	for f in $(find "$1/test/t" -name '*.sh' | LC_ALL=C sort); do
		for name in $(grep -oE '^[[:space:]]*coproc [A-Z_][A-Z0-9_]* \{' "$f" | awk '{ print $2 }' | sort -u); do
			grep -nE "\\\$\\{?${name}_PID\\b" "$f" | grep -vE "^[0-9]+:[[:space:]]*(local )?[A-Za-z_][A-Za-z0-9_]*=\\\$\\{?${name}_PID\\}?([[:space:]]|$)" |
				sed "s|^|${f#"$1/test/t/"}:|; s|\$| <- copy ${name}_PID when the coprocess starts and wait on the copy: bash unsets it once it reaps the coprocess|"
		done
	done
}
if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d "${TMPDIR:-/tmp}/check-tap-coproc.XXXXXX") && trap 'rm -rf "$d"' EXIT
	mkdir -p "$d/test/t"
	# shellcheck disable=SC2016
	printf 'coproc SNAP { psql; }\necho q >&"${SNAP[1]}"\nwait "$SNAP_PID"\n' >"$d/test/t/a.sh"
	# shellcheck disable=SC2016
	printf 'coproc HOLD { psql; }\nholder=$HOLD_PID\nwait "$holder"\n\tlocal p=$HOLD_PID\n' >"$d/test/t/b.sh"
	got=$(check "$d")
	grep -q '^a.sh:3:' <<<"$got" || { echo "selftest: a wait on the pid of a coprocess was not refused: $got"; exit 1; }
	! grep -q '^b.sh:' <<<"$got" || { echo "selftest: a copy of the pid was refused: $got"; exit 1; }
	echo "check-tap-coproc: selftest ok"
	exit 0
fi
out=$(check .)
[[ -z $out ]] || { echo "$out"; exit 1; }
