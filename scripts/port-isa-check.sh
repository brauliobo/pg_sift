#!/bin/sh
# The SQLite and MariaDB baseline policy; callers expand the top-level object glob.
# Usage: port-isa-check.sh LIBRARY OBJECT...
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
[ $bad = 0 ] && echo "isa-check: ok, $n baseline objects without POPCNT or vector instructions, no libgcc popcount helper"
exit $bad
