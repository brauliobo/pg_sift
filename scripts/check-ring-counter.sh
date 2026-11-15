#!/usr/bin/env bash
# A cache of a few entries that replaces the oldest in turn takes its slot from core/ring.h (SIFT_RING_SLOT): an unsigned count and a power of two of slots, checked when it is compiled.
# A count that is incremented and then reduced by hand (x[n++ % N]) is signed or narrow somewhere: after 2^31 uses the signed one is undefined behavior and then a negative remainder,
# an index before the array.
#   scripts/check-ring-counter.sh [DIR]   check the tree under DIR (default: the repo)
#   scripts/check-ring-counter.sh --selftest   an incremented count reduced by a constant is found, a ring slot, a masked cursor and a byte read are not
set -uo pipefail
scan() { # DIR [SOURCE_DIR]
	(cd "$1" && grep -rnE --include='*.c' --include='*.h' '\+\+[ )]*% *\(?([A-Z_][A-Z0-9_]*|[0-9]+)' "${2:-src}" | grep -vE '^[^:]+_test[a-z_]*\.c:' | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(/?\*|//)')
}
if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d) && trap 'rm -rf "$d"' EXIT
	mkdir -p "$d/tree/col"
	printf '%s\n' 'e = &memo[SIFT_RING_SLOT(memo_ring, MEMO_N)];' 'h = (h + 1) & mask;' 'bit = *p++ & 0x80;' ' * the slot is next++ % N, which is signed' >"$d/tree/col/ok.c"
	printf '%s\n' 'r = sift_mix64(rng++) % 100;' >"$d/tree/col/ok_test.c"
	[[ -z $(scan "$d" tree) ]] || { echo "FAIL: a ring slot, a masked cursor, a comment or a test file is reported: $(scan "$d" tree)"; exit 1; }
	for m in 'e = &memo[next++ % MEMO_N];' 'slot = (n++) % 8;' 'e = &t->oc[t->oc_next++ % ORD_CACHE];'; do
		printf '%s\n' "$m" >"$d/tree/col/mutant.c"
		[[ $(scan "$d" tree) == 'tree/col/mutant.c:1:'* ]] || { echo "FAIL: not found: $m"; exit 1; }
	done
	echo "ring-counter selftest ok"; exit 0
fi
bad=$(scan "${1:-$(cd "$(dirname "$0")/.." && pwd)}")
if [[ -n $bad ]]; then
	echo "FAIL: a count incremented and reduced by hand; take the slot with SIFT_RING_SLOT(ring, N) of core/ring.h (a SiftRing and a power of two of slots), which cannot go negative, instead of narrowing or masking the count at the site:"
	sed 's/^/  /' <<<"$bad"
	exit 1
fi
echo "ring-counter: every ring takes its slots through core/ring.h"
