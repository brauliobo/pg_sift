#!/usr/bin/env bash
# The x86-64 baseline of an object or a library, by symbol: outside the functions that the vector levels own there is no POPCNT, BMI, 256 or 512 bit instruction, so that the library runs on
# any x86-64 CPU. The functions that may have them: those of a *_avx2, *_avx512 or *_popcnt object (the cpu dispatch picks them), the clones that core/fmv.h makes
# (name.arch_x86_64_v3, name.arch_x86_64_v4 of an IFUNC, name_x86_v3 and name_x86_v4 of the target() form) and the functions named *_avx2 and *_avx512 of a file that has both kinds.
# Everything else, the .default clone, the *_scalar body and the IFUNC resolver included, has none. (tzcnt is not looked for: GCC writes "rep bsf" for a count of trailing zeros, which is
# bsf on a CPU without BMI and which objdump names tzcnt.)
#
# A library also has: no libgcc popcount helper, no libgcc_s.so in its NEEDED (the resolvers use the libgcc.a that is linked in), no exported IFUNC symbol (the clones are local),
# and FMV clones within a budget.
#
#   scripts/isa-scan.sh [--budget-kib N] FILE...     objects (.o) and libraries (.so); prints the FMV summary of every library
#   scripts/isa-scan.sh --selftest
set -euo pipefail

CLONES='[.]arch_x86[_-]64[_-]v[34]|_x86_v[34]([.]|$)|_avx2([.]|$)|_avx512([.]|$)'

# The functions of a linked library that are the baseline whatever the file they came from: the default clone of a multi-versioned function, its scalar body and its resolver, and the
# detection of the CPU that the resolvers call (libgcc.a: a toolchain whose libgcc is built for x86-64-v3 has instructions in it that no code of ours can remove; it is said and left out)
BASELINE='([.]default([.][0-9]+)?|_scalar|[.]resolver([.]cold)?)$'

libgcc_is_baseline() {
	scan "$(${CC:-cc} -print-libgcc-file-name)" '^__cpu_indicator_init$' >/dev/null 2>&1
}

# the functions of a library that are checked: BASELINE, and the detection of the CPU where the libgcc is baseline
library_only() {
	if libgcc_is_baseline; then echo "$BASELINE|^__cpu_indicator_init\$"; else echo "$BASELINE"; fi
}

# scan FILE [ONLY]: the first instruction of every function outside CLONES that the baseline does not have; of a linked library only the functions that match ONLY (the vector levels
# of the objects cannot be told from the name there)
scan() {
	objdump -d --no-show-raw-insn "$1" | awk -v file="$1" -v clones="$CLONES" -v only="${2:-}" '
	/^[0-9a-f]+ <.*>:$/ { fn = $2; gsub(/[<>:]/, "", fn); skip = (fn ~ clones) || (only != "" && fn !~ only); reported = 0; next }
	/^[ \t]+[0-9a-f]+:/ {
		if (skip || reported) next
		line = $0; sub(/^[ \t]*[0-9a-f]+:[ \t]*/, "", line)
		n = split(line, f, /[ \t]+/); i = 1
		while (i < n && f[i] ~ /^(rep|repz|repnz|lock|notrack|bnd|data16|cs|ds|es|ss|fs|gs)$/) i++
		mn = f[i]
		if (mn ~ /^(popcnt|lzcnt|pdep|pext|bzhi|mulx|rorx|shlx|shrx|sarx|andn|bextr|blsi|blsmsk|blsr)$/ || (mn ~ /^v[a-z]/ && mn !~ /^ver[rw]$/) ||
		    line ~ /%[yz]mm[0-9]+/ || line ~ /%k[0-7]/) {
			printf "%s: %s: %s\n", file, fn, mn; reported = 1; bad = 1
		}
	}
	END { exit bad }'
}

# library_checks SO BUDGET_KIB: what a linked library has besides its instructions
library_checks() {
	local so=$1 budget=$2 bad=0 resolvers clone_bytes
	if ! libgcc_is_baseline; then echo "isa-scan: warning: the libgcc of this toolchain is not x86-64 baseline: __cpu_indicator_init is not checked"; fi
	if nm "$so" | grep -i '__popcount.i2' >/dev/null; then echo "$so: links a libgcc popcount helper"; bad=1; fi
	if objdump -p "$so" | awk '/NEEDED/ { print $2 }' | grep '^libgcc_s\.so' >/dev/null; then echo "$so: needs libgcc_s.so"; bad=1; fi
	if readelf --dyn-syms -W "$so" | awk '$4 == "IFUNC" && $5 == "GLOBAL" && $6 == "DEFAULT" { print "'"$so"': exported IFUNC symbol: " $8; found = 1 } END { exit !found }'; then bad=1; fi
	resolvers=$(readelf -sW "$so" | awk '$4 == "IFUNC"' | wc -l)
	clone_bytes=$(readelf -sW "$so" | awk '$8 ~ /[.]arch_x86[_-]64[_-]v[34]|_x86_v[34]$/ && $4 == "FUNC" { n += $3 } END { print n + 0 }')
	echo "isa-scan: ${so##*/}: $resolvers IFUNC resolvers, $clone_bytes bytes of vector clones (budget $budget KiB)"
	if ((clone_bytes > budget * 1024)); then echo "$so: the vector clones take $clone_bytes bytes, over the budget of $budget KiB"; bad=1; fi
	return $bad
}

selftest() {
	[[ $(uname -m) == x86_64 && $(uname -s) == Linux ]] || { echo "isa-scan selftest: skipped (x86-64 Linux)"; return 0; }
	local d fail=0
	d=$(mktemp -d) && trap 'rm -rf "$d"' RETURN
	build() { cc -O2 -shared -fPIC -march=x86-64 -mtune=generic "${@:2}" -o "$d/$1.so" "$d/$1.c" -lm; }
	expect() { # NAME fail|pass FLAGS...
		local name=$1 want=$2 got=pass
		shift 2
		build "$name" "$@" || { echo "selftest: $name does not build"; fail=1; return; }
		(scan "$d/$name.so" "$(library_only)" && library_checks "$d/$name.so" 150) >"$d/$name.out" 2>&1 || got=fail
		[[ $got == "$want" ]] || { echo "selftest: $name: wanted $want, got $got"; cat "$d/$name.out"; fail=1; }
	}
	printf 'unsigned long f_scalar(unsigned long x) { return __builtin_popcountl(x); }\n' >"$d/leak_popcnt.c"
	expect leak_popcnt fail -mpopcnt
	printf 'unsigned long f(unsigned long x) { return __builtin_popcountl(x); }\n' >"$d/leak_libgcc.c"
	expect leak_libgcc fail
	printf 'typedef float v8 __attribute__((vector_size(32)));\nv8 f_scalar(v8 a, v8 b) { return a + b; }\n' >"$d/leak_ymm.c"
	expect leak_ymm fail -mavx2
	printf '__attribute__((target_clones("default","arch=x86-64-v3"))) int g(const int *a, int n) { int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }\n' >"$d/leak_ifunc.c"
	expect leak_ifunc fail
	printf 'int f(int x) { return x; }\n' >"$d/leak_needed.c"
	expect leak_needed fail -Wl,--no-as-needed -lgcc_s
	printf 'static __attribute__((noinline, target_clones("default","arch=x86-64-v3","arch=x86-64-v4"))) int g(const int *restrict a, int n)\n{ int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }\nint f(const int *a, int n) { return g(a, n); }\n' >"$d/clean_fmv.c"
	expect clean_fmv pass -O3 -fvisibility=hidden
	((fail)) && return 1
	echo "isa-scan selftest: ok, a leaked POPCNT, vector register, libgcc helper, libgcc_s or exported IFUNC fails; a clean multi-versioned function passes"
}

if [[ ${1:-} == --selftest ]]; then selftest; exit; fi
budget=150
if [[ ${1:-} == --budget-kib ]]; then budget=$2; shift 2; fi
[[ $(uname -m) == x86_64 ]] || { echo "isa-scan: skipped, the POPCNT baseline is an x86-64 rule (NEON is part of the AArch64 baseline)"; exit 0; }
bad=0
for f in "$@"; do
	case $f in *_avx2.o | *_avx512.o | *_popcnt.o) continue ;; esac
	case $f in *.so) scan "$f" "$(library_only)" || bad=1 ;; *) scan "$f" || bad=1 ;; esac
	case $f in *.so) library_checks "$f" "$budget" || bad=1 ;; *) nm "$f" 2>/dev/null | grep ' U __popcount' >/dev/null && { echo "$f: calls a libgcc popcount helper"; bad=1; } || true ;; esac
done
exit $bad
