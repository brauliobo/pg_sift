#!/usr/bin/env bash
# sift.approx_count_distinct is an opt-in: an estimate is never what a query gets without asking for it by name (docs/USAGE.md "Approximate distinct counts"). No
# measurement, no server (a grep, about a second). Fails when
#   - a setting or a reloption of src/ turns an exact aggregate into an estimate (a DefineCustom*Variable or add_*_reloption named approx*, other than hll_columns, which only
#     stores sketches for the aggregate that asks),
#   - the count(DISTINCT) compilers (agg/plan_agg.c, agg/dist.c, agg/spec_dist.c) or the planner hook mention the sketch or the estimate,
#   - the test col_approx lacks the proof that count(DISTINCT) is exact with the option on, or the docs lack the error bound, the NULL rule and the merge rule.
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
bad=0
fail() { echo "check-approx: $*"; bad=1; }

hits=$(grep -rnE '(DefineCustom[A-Za-z]*Variable|add_[a-z]*_reloption)\(\s*"[^"]*(approx|hll)' "$R/src" | grep -v '"hll_columns"' || true)
[[ -z $hits ]] || fail "a setting or option other than hll_columns names the estimate: $hits"
for f in agg/plan_agg.c agg/dist.c agg/spec_dist.c agg/plan.c; do
	[[ -f $R/src/$f ]] || continue
	hits=$(grep -nEi 'approx|hll|sketch' "$R/src/$f" || true)
	[[ -z $hits ]] || fail "src/$f is on the path of count(DISTINCT) and names the estimate: $hits"
done
grep -q 'count(DISTINCT' "$R/test/sql/col_approx.sql" && grep -q 'exact_with_option' "$R/test/sql/col_approx.sql" || fail "test/sql/col_approx.sql does not prove count(DISTINCT) exact with hll_columns on"
for p in '0\.81%' 'not counted' 'mergeable' 'is exact in every plan'; do
	grep -qE "$p" "$R/docs/USAGE.md" || fail "docs/USAGE.md lacks '$p' in its section on approximate distinct counts"
done
exit $bad
