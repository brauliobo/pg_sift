#!/bin/sh
# The SQLite and MariaDB baseline policy; callers expand the top-level object glob.
# Usage: port-isa-check.sh LIBRARY OBJECT...
#        port-isa-check.sh --selftest
# Besides the instruction baseline, the library has no symbol that nothing provides: one defined only by the PostgreSQL sources
# (sift_ts_stem, 2026-10-31) loads as "undefined symbol" in the server. What the libc and libm export is versioned (name@GLIBC_x) and a
# weak one is optional; the symbols of the host process itself (the MariaDB server) are listed in HOST_SYMBOLS.
if [ "$1" = --selftest ]; then
	d=$(mktemp -d) && trap 'rm -rf "$d"' EXIT
	printf 'int provided_elsewhere(void);\nint missing_stub(void);\nint f(void) { return provided_elsewhere() + missing_stub(); }\n' >"$d/a.c"
	cc -shared -fPIC -o "$d/a.so" "$d/a.c" || exit 1
	HOST_SYMBOLS=provided_elsewhere sh "$0" "$d/a.so" >"$d/out" 2>&1 && { echo "selftest: an undefined symbol passed"; exit 1; }
	grep -q 'undefined symbol: missing_stub' "$d/out" && ! grep -q provided_elsewhere "$d/out" || { echo "selftest: wrong report"; cat "$d/out"; exit 1; }
	HOST_SYMBOLS="provided_elsewhere missing_stub" sh "$0" "$d/a.so" >/dev/null || { echo "selftest: host symbols rejected"; exit 1; }
	echo "port-isa-check selftest: ok"; exit 0
fi
library=$1
shift

bad=0; n=0
for o do
	case $o in *_avx2.o|*_avx512.o) continue;; esac
	n=$((n + 1))
	if objdump -d $o | grep -qE '\<(popcnt|[yz]mm[0-9]+)\>'; then
		echo "baseline object uses POPCNT or vector registers: $o"; bad=1
	fi
done
if nm $library | grep -qi '__popcount.i2'; then
	echo "${library##*/} links a libgcc popcount helper"; bad=1
fi
for s in $(nm -D --undefined-only $library | awk '$1 == "U" && $2 !~ /@/ {print $2}'); do
	case " $HOST_SYMBOLS " in *" $s "*) continue;; esac
	echo "${library##*/} has undefined symbol: $s"; bad=1
done
[ $bad = 0 ] && echo "isa-check: ok, $n baseline objects without POPCNT or vector instructions, no libgcc popcount helper, no undefined symbol"
exit $bad
