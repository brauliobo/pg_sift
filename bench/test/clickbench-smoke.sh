#!/usr/bin/env bash
# End to end smoke test of bench/steps/clickbench.py on a few thousand rows: a throwaway PostgreSQL cluster (stock heap arm, plus the pg_sift arms when the
# extension is installed in it), a throwaway ClickHouse container (`ch`, and `ch_proj` with --projections on) and DuckDB, each only when it is available.
# Loads, verifies md5_equal on all 43 queries on every arm, times warm and cold, writes the results file, the Markdown block and the SVG. Everything it creates
# is in one scratch directory and one container, removed on exit.
#
#   bench/test/clickbench-smoke.sh [ROWS]       default 20000
#   CH_IMAGE=clickhouse/clickhouse-server:26.9 DUCKDB_BIN=/path/duckdb bench/test/clickbench-smoke.sh
set -euo pipefail
. "$(dirname "$0")/../../scripts/leakcheck.sh"

ROWS=${1:-20000}
STEPS=$(cd "$(dirname "$0")/../steps" && pwd)
TMP=$(mktemp -d /tmp/sift-cbpkg-smoke.XXXXXX)
PGPORT=${SMOKE_PGPORT:-55439} CHPORT=${SMOKE_CHPORT:-18129} CH=sift-cbpkg-ch-$$
CH_IMAGE=${CH_IMAGE:-clickhouse/clickhouse-server:26.9.19.9}
DUCKDB_BIN=${DUCKDB_BIN:-$(command -v duckdb || true)}
PGBIN=$(pg_config --bindir)
export PGHOST=$TMP PGPORT SIFT_TMP=$TMP PGUSER=${USER:-postgres} PYTHONDONTWRITEBYTECODE=1

cleanup() {
	docker rm -f "$CH" >/dev/null 2>&1 || true
	"$PGBIN/pg_ctl" -D "$TMP/pg" -m immediate stop >/dev/null 2>&1 || true
	rm -rf "$TMP"
}
trap cleanup EXIT

"$PGBIN/initdb" -D "$TMP/pg" -A trust -U "$PGUSER" >/dev/null
"$PGBIN/pg_ctl" -D "$TMP/pg" -w -l "$TMP/postgres.log" -o "-p $PGPORT -k $TMP -c listen_addresses= -c shared_buffers=256MB" start >/dev/null
leak_baseline "$TMP/pg"

arms=pg
args=(--work "$TMP/work" --scale "$ROWS" --pg-datadir "$TMP/pg" --threads 2)
[[ $(psql -X -qAt -d postgres -c "SELECT count(*) FROM pg_available_extensions WHERE name = 'pg_sift'") == 1 ]] && has_sift=1 || has_sift=0
if ((has_sift)); then
	psql -X -q -d postgres -c 'CREATE DATABASE smoke_probe'
	psql -X -q -d smoke_probe -c 'CREATE EXTENSION pg_sift' 2>/dev/null || true
	[[ $(psql -X -qAt -d smoke_probe -c "SELECT count(*) FROM pg_am WHERE amname = 'sift_col'" 2>/dev/null) == 1 ]] && arms+=,sift_col
	[[ $(psql -X -qAt -d smoke_probe -c "SELECT to_regproc('sift.table_set_option') IS NOT NULL" 2>/dev/null) == t ]] && arms+=,sift_table
fi
if docker image inspect "$CH_IMAGE" >/dev/null 2>&1 || docker pull -q "$CH_IMAGE" >/dev/null 2>&1; then
	docker run -d --rm --name "$CH" -p "127.0.0.1:$CHPORT:8123" -e CLICKHOUSE_SKIP_USER_SETUP=1 "$CH_IMAGE" >/dev/null
	until curl -fs "http://127.0.0.1:$CHPORT/ping" >/dev/null; do sleep 0.5; done
	arms+=,ch
	arms+=,ch_proj
	args+=(--ch-url "http://127.0.0.1:$CHPORT" --ch-container "$CH")
fi
if [[ -n $DUCKDB_BIN ]]; then arms+=,duckdb; args+=(--duckdb-bin "$DUCKDB_BIN"); fi
echo "smoke: arms $arms, $ROWS rows"

python3 "$STEPS/clickbench.py" parse-check "${args[@]}"
source=(--source synthetic)
if [[ ${SMOKE_SOURCE:-synthetic} == parquet ]]; then
	# a file shaped like the real hits.parquet, made from the synthetic rows: the converter that feeds every arm is exercised
	mkdir -p "$TMP/fixture"
	psql -X -qAt -d postgres -v "n=$ROWS" -f "$STEPS/../sql/clickbench/gen.sql" >"$TMP/fixture/hits.csv"
	(cd "$STEPS" && python3 -c "import clickbench_schema as s; print(s.parquet_fixture_sql('$TMP/fixture/hits.csv', '$TMP/fixture/hits.parquet'))") |
		env -u AI_AGENT -u CLAUDECODE "$DUCKDB_BIN" -noheader :memory:
	source=(--source parquet --parquet "$TMP/fixture/hits.parquet")
fi
python3 "$STEPS/clickbench.py" load --arms "$arms" "${source[@]}" "${args[@]}"
python3 "$STEPS/clickbench.py" run --arms "$arms" --mode "${SMOKE_MODE:-both}" --runs 2 --cold-runs 1 --out "$TMP/clickbench.tsv" "${args[@]}"
python3 "$STEPS/clickbench.py" report --in "$TMP/clickbench.tsv" --md "$TMP/BENCHMARKS.md" --svg "$TMP/clickbench.svg"
[[ -s $TMP/clickbench.svg && -s $TMP/BENCHMARKS.md ]]
grep -c $'\tmd5_equal\t1\t' "$TMP/clickbench.tsv" | sed 's/^/smoke: md5_equal rows equal: /'
! grep -q $'\tmd5_equal\t0\t' "$TMP/clickbench.tsv"
if [[ -n ${SMOKE_KEEP:-} ]]; then mkdir -p "$SMOKE_KEEP" && cp "$TMP/clickbench.tsv" "$TMP/BENCHMARKS.md" "$TMP/clickbench.svg" "$SMOKE_KEEP/"; fi
leak_end "$TMP/pg" "$TMP" "$PGBIN/pg_ctl" -D "$TMP/pg" -m fast -w stop >/dev/null
echo "smoke: ok"
