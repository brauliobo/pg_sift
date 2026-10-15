#!/usr/bin/env bash
# The fallback path must run on any x86-64: outside the AVX2 and AVX-512 objects there is no
# POPCNT instruction, and nothing links libgcc's __popcountdi2 (a distribution may build it with
# POPCNT). Run after a build.
#
#   scripts/isa_check.sh
set -eu
cd "$(dirname "$0")/.."

bad=0 checked=0 skipped=0
for o in src/*.o; do
	case $o in *_avx2.o | *_avx512.o) skipped=$((skipped + 1)); continue ;; esac
	checked=$((checked + 1))
	insns=$(objdump -d "$o" | grep -cw popcnt || true)
	libcalls=$(nm "$o" 2>/dev/null | grep -c ' U __popcount' || true)
	if ((insns + libcalls)); then
		echo "baseline object uses POPCNT: $o"
		bad=1
	fi
done
if [[ $(nm sift.so | grep -ci '__popcount.i2' || true) != 0 ]]; then
	echo "sift.so links a libgcc popcount helper"
	bad=1
fi
((checked)) || { echo "isa_check: no objects in src/, build first"; exit 1; }
if ((bad)); then echo "isa_check: FAIL"; else echo "isa_check: ok, $checked baseline objects without POPCNT or libgcc popcount calls ($skipped AVX2/AVX-512 objects skipped), sift.so links no libgcc popcount helper"; fi
exit $bad
