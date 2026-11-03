#!/usr/bin/env bash
# Scratch space is memory. A throwaway cluster or temporary directory on a disk wears it all day, so the tests, the benchmarks and the scripts do not make one:
#   - no '/var/tmp' anywhere in scripts, test or bench;
#   - no ~/.cache, $HOME/.cache or XDG_CACHE_HOME in them, except the files of scripts/scratch.allow (a cache that is built once and read, with its reason);
#   - a Python file that makes a temporary directory or file without dir= imports tmpfs (bench/steps/tmpfs.py), which puts tempfile on tmpfs.
# scripts/check-scratch.sh [--selftest]
cd "$(git rev-parse --show-toplevel)" || exit 1
allow=scripts/scratch.allow
check() { # ROOT: prints a line for each breach under ROOT
	local root=$1 f
	git -C "$root" grep -nE '/var/tmp' -- scripts test bench ':!scripts/check-scratch.sh' ':!scripts/scratch.allow' | sed 's/$/  (use tmpfs: bench\/steps\/tmpfs.py or \/dev\/shm)/'
	git -C "$root" grep -lE '(\$HOME|~)/\.cache|XDG_CACHE_HOME' -- scripts test bench ':!scripts/check-scratch.sh' ':!scripts/scratch.allow' | while read -r f; do
		grep -q "^$f	" "$root/$allow" || echo "$f: a cache directory of the user as scratch (a cache that is built once goes in $allow with its reason; scratch goes under /dev/shm)"
	done
	git -C "$root" grep -lE 'tempfile\.(mkdtemp|TemporaryDirectory|mkstemp|NamedTemporaryFile)\(' -- 'scripts/*.py' 'test/*.py' 'bench/*.py' | while read -r f; do
		grep -q "^$f	" "$root/$allow" || grep -qE '^[^#]*import tmpfs|dir=' "$root/$f" || echo "$f: makes temporary files without dir= and does not import tmpfs (bench/steps/tmpfs.py)"
	done
}
if [[ ${1:-} == --selftest ]]; then
	t=$(mktemp -d "$([[ -d /dev/shm ]] && echo /dev/shm || echo "${TMPDIR:-/tmp}")/check-scratch.XXXXXX") || exit 1   # memory where there is a tmpfs, the temporary directory of macOS
	trap 'rm -rf "$t"' EXIT
	git -C "$t" init -q && mkdir -p "$t/scripts" "$t/bench" "$t/test" && : >"$t/$allow"
	echo 'D=/var/tmp/x' >"$t/scripts/a.sh"
	echo 'C=$HOME/.cache/x' >"$t/scripts/b.sh"
	printf 'import tempfile\nd = tempfile.mkdtemp()\n' >"$t/bench/c.py"
	printf 'import tmpfs\nimport tempfile\nd = tempfile.mkdtemp()\n' >"$t/bench/ok.py"
	printf 'scripts/b.sh\tbuilt once\n' >"$t/$allow"
	git -C "$t" add -A
	out=$(check "$t")
	for want in 'scripts/a.sh' 'bench/c.py'; do grep -q "$want" <<<"$out" || { echo "selftest: $want was not refused: $out"; exit 1; }; done
	if grep -qE 'scripts/b.sh|bench/ok.py' <<<"$out"; then echo "selftest: an allowed or clean file was refused: $out"; exit 1; fi
	echo "check-scratch selftest ok"
	exit 0
fi
out=$(check .)
[[ -z $out ]] || { echo "$out"; exit 1; }
echo "scratch: none on a disk"
