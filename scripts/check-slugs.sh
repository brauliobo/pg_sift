#!/usr/bin/env bash
# Query, case and shape identifiers are descriptive slugs, never bare codes (q05, j12, G7, a01_...): lower case, [a-z0-9_-], a hyphen or underscore or five
# letters, and no segment of one or two letters and a number. The ids are the file names of bench/cases, bench/sql/*/queries and bench/sql/workload/shapes,
# the slugs of bench/sql/analytics/clickbench.tsv and the `-- name:` of test/plan/shapes.sql.
set -euo pipefail
cd "$(dirname "$0")/.."
ids() {
	git ls-files 'bench/cases/*.sql' 'bench/sql/*/queries/*.sql' 'bench/sql/workload/shapes/*.sql' | sed 's|.*/||; s|\.sql$||'
	cut -f1 bench/sql/analytics/clickbench.tsv | grep -v '^#'
	sed -n 's/^-- name: //p' test/plan/shapes.sql
}
bad=$({ ids | grep -vE '^([a-z]{5,}|[a-z][a-z0-9]*[_-][a-z0-9_-]+)$' || true; ids | grep -E '(^|[_-])[a-z]{1,2}[0-9]+([_-]|$)' || true; } | sort -u)
labels=$(git grep -nE '^-- label: [^`]*\b(Q[0-9]+|[GS][1-9])\b' -- 'bench/cases/*.sql' 'bench/sql/*/queries/*.sql' || true)   # a suite's number belongs in the header comment
[[ -z $labels ]] || { printf 'a label that starts with a code: %s\n' "$labels" >&2; exit 1; }
[[ -z $bad ]] || { printf 'not a descriptive slug: %s\n' $bad >&2; exit 1; }
echo "check-slugs: every query, case and shape id is a slug"
