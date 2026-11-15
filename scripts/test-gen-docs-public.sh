#!/usr/bin/env bash
# scripts/gen-docs.py --check without SIFT_PUBLIC_TREE fails on a benchmark block whose results file is missing or stale; with SIFT_PUBLIC_TREE=true (the public repository, whose
# results stay private) a block whose results file is not in the tree is not compared, while a block that its results file backs is. Works on a copy of the tree.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2
[[ ${SIFT_PUBLIC_TREE:-} != true ]] || { echo "gen-docs-public: skipped, the public tree does not hold the results file the cases are built on"; exit 0; }
bad=0
fail() { echo "FAIL: $*"; bad=1; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf - 2>/dev/null | tar -xf - -C "$T"
git -C "$T" init -q
git -C "$T" add -A
unset CI SIFT_LINT_NOCACHE SIFT_PUBLIC_TREE
export SIFT_GEN_DOCS_CACHE=$T/.cache
check() { (cd "$T" && env "$@" python3 -B -I scripts/gen-docs.py --check >"$T/check.log" 2>&1); }
FILE=scope-stock.tsv
RESULTS=docs/results/$FILE
BLOCK=scope-stock-readme
[[ -f $T/$RESULTS ]] || { echo "FAIL: the tree has no $RESULTS to build the cases on"; exit 1; }
grep -q "bench:$BLOCK:begin" "$T/README.md" || { echo "FAIL: README.md has no $BLOCK block to build the cases on"; exit 1; }

check || fail "a complete tree is stale: $(tail -n 2 "$T/check.log")"
check SIFT_PUBLIC_TREE=true || fail "a complete tree is stale in the public check: $(tail -n 2 "$T/check.log")"
# a block that its results file backs and that no longer matches it is stale, in both checks
n=$(awk "/<!-- bench:$BLOCK:begin/ { f = 1; next } f && NF && !d { print NR; d = 1 }" "$T/README.md")
sed -i.bak "${n}s/\$/ stale/" "$T/README.md" && rm -f "$T/README.md.bak"
check && fail "a stale block was accepted"
check SIFT_PUBLIC_TREE=true && fail "a stale block that its results file backs was accepted in the public check"
git -C "$T" checkout -q README.md
# the results file is not in the tree: stale in the development check, not compared in the public one
git -C "$T" rm -q -f "$RESULTS"
check && fail "a block without its results file was accepted in the development check"
check SIFT_PUBLIC_TREE=true || fail "a block without its results file was refused in the public check: $(tail -n 2 "$T/check.log")"
(( bad )) || echo "gen-docs-public: the development check refuses a missing or stale block, the public check skips the ones whose results are private"
exit $bad
