#!/usr/bin/env bash
# The SIMD level this machine runs, on the record of every CI job: the CPU, the level its flags promise (the rule of src/core/cpu.c) and the level the
# library detected. The kernel tests pass at a level the CPU lacks (sift.simd falls back to the best supported one), so a runner without AVX-512
# runs AVX-512 nowhere, and only this line says so. Fails when the library detects less than the flags promise, or less than SIFT_EXPECT_SIMD
# (scalar|avx2|avx512|neon) when a job names the level it must run. Run from scripts/ci.sh after the build; needs the staged build (SIFT_ROOT).
set -euo pipefail
S=$(cd "$(dirname "$0")" && pwd)

flags() { sed -n '/^\(flags\|Features\)[[:space:]]*:/{s/^[^:]*:[[:space:]]*//p;q}' /proc/cpuinfo 2>/dev/null || true; }

model=$(sed -n '/^model name[[:space:]]*:/{s/^[^:]*:[[:space:]]*//p;q}' /proc/cpuinfo 2>/dev/null || true)
[[ -n $model ]] || model=$(sed -n '/^CPU part[[:space:]]*:/{s/^[^:]*:[[:space:]]*/arm cpu part /p;q}' /proc/cpuinfo 2>/dev/null || true)
[[ -n $model ]] || model=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m)
have=" $(flags) "
has() { local f; for f in "$@"; do [[ $have == *" $f "* ]] || return 1; done; }

case $(uname -m) in
	x86_64)
		promised=scalar
		if has avx2 bmi2 popcnt; then
			promised=avx2
			has avx512f avx512bw avx512vl avx512dq avx512_vpopcntdq avx512_vbmi2 && promised=avx512
		fi ;;
	aarch64|arm64) promised=neon ;;
	*) promised=scalar ;;
esac

export SIFT_TMP=${SIFT_TMP:?}/simd-level
trap '"$S/pgtest.sh" stop >/dev/null 2>&1 || true' EXIT
"$S/pgtest.sh" start >/dev/null
"$S/pgtest.sh" psql -qAt -c 'CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE' >/dev/null
detected=$("$S/pgtest.sh" psql -qAt -c 'SELECT sift._test_simd_detected()')

line="simd: cpu '$model', flags promise $promised, library detected $detected$(
	[[ $promised != avx2 ]] || ! has avx512f || echo " (AVX-512 flags present: $(for f in avx512bw avx512vl avx512dq avx512_vpopcntdq avx512_vbmi2; do has $f || printf '%s missing ' "$f"; done))")"
echo "$line"
[[ -z ${GITHUB_STEP_SUMMARY:-} ]] || echo "- $line" >>"$GITHUB_STEP_SUMMARY"

rank() { case $1 in scalar) echo 0 ;; avx2|neon) echo 1 ;; avx512) echo 2 ;; esac; }
[[ $detected == "$promised" ]] || { echo "simd: the flags promise $promised and the library detected $detected" >&2; exit 1; }
if [[ -n ${SIFT_EXPECT_SIMD:-} ]] && (( $(rank "$detected") < $(rank "$SIFT_EXPECT_SIMD") )); then
	echo "simd: this job must run $SIFT_EXPECT_SIMD and the machine runs $detected" >&2
	exit 1
fi
