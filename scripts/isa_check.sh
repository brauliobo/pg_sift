#!/usr/bin/env bash
# The fallback path must run on any x86-64: outside the functions the cpu dispatch and the multi-versioned loops of core/fmv.h pick (scripts/isa-scan.sh says which) there is no POPCNT, BMI,
# 256 or 512 bit instruction, nothing links libgcc's __popcountdi2 (a distribution may build it with POPCNT) or libgcc_s, and no IFUNC symbol is exported. Run after a build.
#
#   scripts/isa_check.sh                  the objects of src/ and pg_sift.so of the build tree
#   scripts/isa_check.sh --so LIBRARY     an installed library (a package test)
#   scripts/isa_check.sh --selftest       the check itself, on synthetic libraries
set -eu
cd "$(dirname "$0")/.."

case ${1:-} in
--selftest) exec scripts/isa-scan.sh --selftest ;;
--so) exec scripts/isa-scan.sh --budget-kib 150 "$2" ;;
esac

[[ $(uname -m) == x86_64 ]] || { echo "isa_check: skipped, the POPCNT baseline is an x86-64 rule (NEON is part of the AArch64 baseline)"; exit 0; }
objects=(src/*/*.o src/*/*/*.o src/*/*/*/*.o)
[[ -e ${objects[0]} ]] || { echo "isa_check: no objects in src/, build first"; exit 1; }
scripts/isa-scan.sh --budget-kib 150 "${objects[@]}" pg_sift.so || { echo "isa_check: FAIL"; exit 1; }
echo "isa_check: ok, ${#objects[@]} objects and pg_sift.so have no POPCNT, BMI or vector instruction outside the vector levels and the clones of core/fmv.h, no libgcc popcount helper, no libgcc_s, no exported IFUNC"
