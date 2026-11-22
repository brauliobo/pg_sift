#!/usr/bin/env bash
# A library with no GNU IFUNC symbol and no IRELATIVE relocation: what musl (Alpine) needs, whose loader refuses R_*_IRELATIVE at dlopen, and what a SIFT_FMV=off build must be.
#
#   scripts/check-no-ifunc.sh LIBRARY.so
#   scripts/check-no-ifunc.sh --selftest
set -euo pipefail

if [[ ${1:-} == --selftest ]]; then
	[[ $(uname -s) == Linux ]] || { echo "check-no-ifunc selftest: skipped (ELF libraries)"; exit 0; }
	d=$(mktemp -d) && trap 'rm -rf "$d"' EXIT
	printf 'static __attribute__((noinline, target_clones("default","arch=x86-64-v3"))) int g(const int *restrict a, int n)\n{ int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }\nint f(const int *a, int n) { return g(a, n); }\n' >"$d/ifunc.c"
	printf 'int f(const int *a, int n) { int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }\n' >"$d/plain.c"
	if [[ $(uname -m) == x86_64 ]]; then
		cc -O2 -shared -fPIC -fvisibility=hidden -o "$d/ifunc.so" "$d/ifunc.c"
		"$0" "$d/ifunc.so" >/dev/null 2>&1 && { echo "selftest: a library with an IFUNC passed"; exit 1; }
	fi
	cc -O2 -shared -fPIC -o "$d/plain.so" "$d/plain.c"
	"$0" "$d/plain.so" >/dev/null || { echo "selftest: a library without an IFUNC failed"; exit 1; }
	echo "check-no-ifunc selftest: ok"; exit 0
fi

so=${1:?usage: check-no-ifunc.sh LIBRARY.so}
if readelf -rW "$so" | grep 'IRELATIVE' >/dev/null; then
	echo "check-no-ifunc: FAIL: $so has IRELATIVE relocations (dlopen fails on musl)" >&2
	readelf -rW "$so" | grep 'IRELATIVE' | head -n 10 >&2
	exit 1
fi
if readelf -sW "$so" | awk '$4 == "IFUNC" { found = 1; print } END { exit !found }' >&2; then
	echo "check-no-ifunc: FAIL: $so has STT_GNU_IFUNC symbols" >&2
	exit 1
fi
echo "check-no-ifunc: ok, ${so##*/} has no IFUNC symbol and no IRELATIVE relocation"
