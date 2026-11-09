#!/usr/bin/env bash
# Every undefined dynamic symbol of the library is exported by the server binary, by libc, libm or by a library the library names (DT_NEEDED). `-shared` links with
# undefined symbols, and the loader refuses the library only when the server opens it: an include trim that empties a NEON file on AArch64 (SIFT_HAVE_NEON comes from
# core/cpu.h) shows up as hundreds of failing tests, not as the one symbol it is. Runs after the link, in `pgtest.sh install` (so in the build step of scripts/ci.sh).
#   scripts/check-undefined.sh LIB [POSTGRES]   default server: the postgres of `pg_config --bindir`; prints `undefined: SYMBOL`, exit 1 if any (Linux only)
#   scripts/check-undefined.sh --selftest
set -euo pipefail

exported() { nm -D --defined-only "$@" | awk 'NF >= 3 { sub(/@.*/, "", $3); print $3 }' | sort -u; }

# check LIB POSTGRES: the undefined symbols (weak ones may stay undefined) that nothing the loader has at hand defines
check() {
	local libs=() l p
	for l in libc.so.6 libm.so.6 $(readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
		p=$(${CC:-cc} -print-file-name="$l")
		[[ $p == /* ]] || { echo "check-undefined: $l: not found by ${CC:-cc}" >&2; return 2; }
		libs+=("$p")
	done
	comm -23 <(nm -D --undefined-only "$1" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) <(exported "$2" "${libs[@]}")
}

selftest() {
	local t; t=$(mktemp -d); trap "rm -rf $t" RETURN
	echo 'int pg_exported(void) { return 1; } int main(void) { return 0; }' >"$t/pg.c"
	printf '#include <stdlib.h>\nextern int pg_exported(void);\nvoid *f(void) { return malloc(pg_exported()); }\n' >"$t/ok.c"
	printf 'extern int sift_no_such_symbol(void);\nint g(void) { return sift_no_such_symbol(); }\n' >"$t/bad.c"
	${CC:-cc} -rdynamic -o "$t/pg" "$t/pg.c"
	${CC:-cc} -shared -fPIC -o "$t/ok.so" "$t/ok.c"
	${CC:-cc} -shared -fPIC -o "$t/bad.so" "$t/ok.c" "$t/bad.c"
	[[ -z $(check "$t/ok.so" "$t/pg") ]] || { echo "selftest: a library whose symbols all have a provider was refused"; return 1; }
	[[ $(check "$t/bad.so" "$t/pg") == sift_no_such_symbol ]] || { echo "selftest: an undefined symbol no one provides was accepted"; return 1; }
	echo 'check-undefined selftest: ok'
}

[[ $(uname) == Linux ]] || { echo "check-undefined: ELF libraries only, nothing to check on $(uname)"; exit 0; }
[[ ${1:-} != --selftest ]] || { selftest; exit; }
lib=${1:?usage: check-undefined.sh LIB [POSTGRES]}
missing=$(check "$lib" "${2:-$(pg_config --bindir)/postgres}")
[[ -z $missing ]] || { sed 's/^/undefined: /' <<<"$missing"; exit 1; }
echo "check-undefined: $(basename "$lib"): every undefined symbol is provided by the server or the libraries it names"
