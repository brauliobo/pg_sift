#!/usr/bin/env bash
# The fallback path must run on any x86-64: outside the AVX2 and AVX-512 objects there is no
# POPCNT instruction, and nothing links libgcc's __popcountdi2 (a distribution may build it with
# POPCNT). Run after a build.
#
#   scripts/isa_check.sh
set -eu
cd "$(dirname "$0")/.."

bad=0
for o in src/*.o; do
	case $o in *_avx2.o | *_avx512.o) continue ;; esac
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
exit $bad
