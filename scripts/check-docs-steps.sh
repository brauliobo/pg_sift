#!/usr/bin/env bash
# The numbered steps of a fenced block in the docs ("# 1. install", "# 2. restart", ...) start at 1 and are consecutive: a lost step shows as a gap (the
# Upgrading page once lost its step 3 and a stray comment line stood in its place).
#
#   scripts/check-docs-steps.sh              every tracked *.md
#   scripts/check-docs-steps.sh --selftest   the check on a block with a gap and on one without
set -euo pipefail
cd "$(dirname "$0")/.."

PROG='
	/^[ ]*```/ { fence = !fence; last = 0; next }
	fence && /^# [0-9]+\. / {
		n = $2 + 0
		if (n != last + 1) { printf "%s:%d: step %d follows step %d in a fenced block\n", FILENAME, FNR, n, last; bad = 1 }
		last = n
	}
	END { exit bad }'

if [[ ${1:-} == --selftest ]]; then
	dir=$(mktemp -d "${TMPDIR:-/tmp}/docs-steps.XXXXXX")
	trap 'rm -rf "$dir"' EXIT
	printf '```\n# 1. a\n# 2. b\n# 4. d\n```\n' >"$dir/gap.md"
	printf '```\n# 1. a\n# 2. b\n```\n```\n# 1. again\n```\n' >"$dir/ok.md"
	! awk "$PROG" "$dir/gap.md" >/dev/null || { echo "check-docs-steps: a gap passed"; exit 1; }
	awk "$PROG" "$dir/ok.md" || { echo "check-docs-steps: consecutive steps failed"; exit 1; }
	echo "check-docs-steps: selftest ok"
	exit 0
fi
git ls-files -z '*.md' | xargs -0 awk "$PROG"
echo "check-docs-steps: the numbered steps of every fenced block are consecutive"
