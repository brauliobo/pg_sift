#!/usr/bin/env bash
# The corpus gate (test/corpus/README.md): the 312 statements of test/corpus/cases.sql on the lab of test/corpus/lab.sql (20000 events), each in a new backend with sift off
# and on, the rows equal, and the plan slug of each (the Custom Scan (Sift...) nodes and the verdict of sift.explain_why per arm) the one pinned in test/corpus/plans.tsv.
#
#   scripts/corpus.sh [--only GLOB] [--rows N] [--update] [--keep]
#
#   --only GLOB   only the statements whose slug matches (a focused run: the lab is loaded as ever)
#   --rows N      events of the lab (default 20000, the size the plans are pinned at: another size checks the rows only)
#   --update      rewrites test/corpus/plans.tsv from this run: read the diff, a statement that left the engaged set is a regression unless the change says so
#   --keep        keeps the database sift_corpus
#   CORPUS_ON_SET='set sift.x=off;'   SQL run in each sift-on session after its settings: what a setting does to the rows and the plans
# Runs on the cluster of scripts/pgtest.sh (SIFT_TMP, SIFT_PORT; installed and started) like scripts/exec-budget.sh; scripts/corpus-local.sh brings its own.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
ROWS=20000 ONLY='*' UPDATE= KEEP=
while (( $# )); do
	case $1 in
		--only) ONLY=$2; shift ;; --rows) ROWS=$2; shift ;; --update) UPDATE=1 ;; --keep) KEEP=1 ;;
		*) sed -n 2,12p "$0"; exit 1 ;;
	esac
	shift
done
eval "$("$REPO/scripts/pgtest.sh" env)"
export PGUSER=postgres PGDATABASE=postgres
pg_isready -q -t 2 || { echo "corpus: no server at SIFT_TMP=$SIFT_TMP SIFT_PORT=${SIFT_PORT:-} (scripts/corpus-local.sh installs and starts a throwaway one)" >&2; exit 1; }
DB=sift_corpus
cleanup() {
	[[ -n $KEEP ]] || psql -X -qAt -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" >/dev/null 2>&1 || true
}
trap cleanup EXIT
t0=$SECONDS
psql -X -qAt -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" -c "CREATE DATABASE $DB" >/dev/null
export PGDATABASE=$DB PGOPTIONS="${PGOPTIONS:-} -c client_min_messages=warning"
psql -X -q -v ON_ERROR_STOP=1 -v n_events="$ROWS" -c "CREATE EXTENSION pg_sift" -f "$REPO/test/corpus/lab.sql" >/dev/null
echo "corpus: lab of $ROWS events loaded in $((SECONDS - t0)) s" >&2
t1=$SECONDS
args=(--only "$ONLY"); [[ -z $UPDATE ]] || args+=(--update); [[ $ROWS == 20000 ]] || args+=(--no-plans)
rc=0
python3 -I "$REPO/test/corpus/run.py" "${args[@]}" || rc=$?
echo "corpus: statements run in $((SECONDS - t1)) s" >&2
exit $rc
