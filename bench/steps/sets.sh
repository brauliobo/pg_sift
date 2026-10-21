#!/usr/bin/env bash
# Usage: bench/bench sets [--docs] [--ref TAG | --build DIR --root DIR] [--reps N] [--no-clickhouse] [--facts N] [--entities N] [--out FILE] [--keep]   (steps/sets.sh)
# The set-filter benchmark: a fact table filtered by 1k / 2k / 5k entity ids (10k and 100k as headroom), as an array parameter on a generic plan, as a literal IN list, on the normal
# plan, as IN (SELECT ... FROM membership), and on ClickHouse.
#   --build DIR   the checkout whose sift is measured (default: this one; its scripts/pgtest.sh stages it): `git worktree add /tmp/x vX.Y.Z` measures a release
#   --root DIR    a staged install of that build (pgtest.sh install, SIFT_TESTING= for the release library) instead of building it
#   --docs        also write bench/results/sets.tsv and the generated block of docs/BENCHMARKS.md
#   --keep        keep the scratch cluster (the data is also cached as a dump in $BENCH_STORE/cache, so a rerun only restores it)
# Everything runs on a scratch cluster of its own under $SCRATCH_ROOT in a systemd scope (12G, 3 CPUs), ClickHouse in a container of 3 CPUs and 8g; the timed part
# takes /tmp/sift-perf.lock (blocking), and the host load is recorded with every cell. The data is bench/sql/sets/gen.sql (deterministic).
set -euo pipefail
export LC_ALL=C
HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(dirname "$HERE")
REPO=$(dirname "$BENCH")
. "$HERE/scratch.sh"
. "$BENCH/versions.env"
. "$HERE/builds.sh"
STORE=${BENCH_STORE:-${BENCH_WORK:-/tmp/sift-bench}}

ORIG=("$@")
REF='' DOCS='' REPS=9 CH=1 NF=3000000 NE=200000 OUT='' KEEP=${KEEP:-}
while (( $# )); do
	case $1 in
		--docs) DOCS=1 ;;
		--build) SIFT_BUILD=$(cd "$2" && pwd); shift ;;
		--ref) REF=$2; shift ;;   # a release of sift built once under $EXTRAS_SRC (steps/builds.sh), instead of --build and --root
		--root) export SIFT_ROOT=$2; shift ;;
		--reps) REPS=$2; shift ;;
		--no-clickhouse) CH='' ;;
		--facts) NF=$2; shift ;;
		--entities) NE=$2; shift ;;
		--out) OUT=$2; shift ;;
		--keep) KEEP=1 ;;
		*) sed -n '2,12p' "$0"; exit 1 ;;
	esac
	shift
done
export KEEP
if [[ -n $REF ]]; then L=$(build_label "$REF"); build_prepare "$L"; SIFT_BUILD=$(build_dir "$L"); export SIFT_ROOT=$EXTRAS_CACHE/$L/root; fi
scratch_scope "${ORIG[@]}"

OUT=${OUT:-$BENCH/out/sets-$(date -u +%Y%m%dT%H%M%S).tsv}
mkdir -p "$(dirname "$OUT")" "$STORE/cache"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/sift-sets.XXXXXX")
CHC=sift-sets-ch-$$
cleanup() { docker rm -f "$CHC" >/dev/null 2>&1 || true; scratch_down; rm -rf "$WORK"; }
trap cleanup EXIT

scratch_up sets
DUMP=$STORE/cache/sets-$NF-$NE.dump
sq -d postgres -c "DROP DATABASE IF EXISTS sets" -c "CREATE DATABASE sets" >/dev/null
sq -d sets -c "CREATE EXTENSION pg_sift" >/dev/null
echo "sets: $(sq -d sets -c 'SELECT sift.version()') (build $(git -C "$SIFT_BUILD" rev-parse --short HEAD)) on port $SIFT_PORT" >&2
if [[ -f $DUMP ]]; then
	echo "sets: restoring $DUMP" >&2
	pg_restore -d sets --no-owner "$DUMP"
else
	echo "sets: generating $NF facts of $NE dim_a" >&2
	sq -d sets -v nf="$NF" -v ne="$NE" -f "$BENCH/sql/sets/gen.sql" >/dev/null
	pg_dump -Fc -d sets -t facts -t dim_a -t memberships -f "$DUMP.tmp" && mv "$DUMP.tmp" "$DUMP"
fi
echo "sets: indexes" >&2
sq -d sets >/dev/null <<-'EOF'
	SET maintenance_work_mem = '512MB';
	SET max_parallel_maintenance_workers = 2;
	CREATE INDEX facts_entity_date ON facts (dim_a_id, fact_date);
	CREATE INDEX facts_date ON facts (fact_date);
	CREATE INDEX facts_col ON facts USING sift_col (dim_a_id, foreign_id, lang_id, signal, fact_date, score, deleted_at, (date_trunc('month', fact_date::timestamp)::date));
	CREATE INDEX IF NOT EXISTS memberships_group_entity ON memberships (group_id, dim_a_id);
	-- the same facts with the entity key as int8, text and date: the arrays of the key-type rows
	CREATE TABLE facts_by_int8 AS SELECT id, dim_a_id::int8 AS dim_a_id, foreign_id, lang_id, signal, fact_date, score, deleted_at FROM facts;
	CREATE TABLE facts_by_text AS SELECT id, 'ent-' || lpad(dim_a_id::text, 7, '0') AS dim_a_id, foreign_id, lang_id, signal, fact_date, score, deleted_at FROM facts;
	CREATE TABLE facts_by_date AS SELECT id, date '2000-01-01' + dim_a_id AS dim_a_id, foreign_id, lang_id, signal, fact_date, score, deleted_at FROM facts;
	SELECT format('CREATE INDEX %1$s_entity_date ON %1$s (dim_a_id, fact_date)', t) FROM unnest(ARRAY['facts_by_int8', 'facts_by_text', 'facts_by_date']) t \gexec
	SELECT format('CREATE INDEX %1$s_date ON %1$s (fact_date)', t) FROM unnest(ARRAY['facts_by_int8', 'facts_by_text', 'facts_by_date']) t \gexec
	SELECT format('CREATE INDEX %1$s_col ON %1$s USING sift_col (dim_a_id, foreign_id, lang_id, signal, fact_date, score, deleted_at, (date_trunc(''month'', fact_date::timestamp)::date))', t)
		FROM unnest(ARRAY['facts_by_int8', 'facts_by_text', 'facts_by_date']) t \gexec
	VACUUM (ANALYZE) facts_by_int8, facts_by_text, facts_by_date;
	VACUUM (ANALYZE) facts, dim_a, memberships;
EOF
echo "sets: facts $(sq -d sets -c "SELECT pg_size_pretty(pg_relation_size('facts'))") heap, sift_col $(sq -d sets -c "SELECT pg_size_pretty(pg_relation_size('facts_col'))")" >&2

if [[ -n $CH ]]; then
	if ! docker info >/dev/null 2>&1; then echo "sets: no docker, ClickHouse skipped" >&2; CH=''; fi
fi
if [[ -n $CH ]]; then
	docker run -d --rm --name "$CHC" --cpus 3 --memory 8g "$STORE_CH_IMAGE" >/dev/null
	for _ in $(seq 1 60); do docker exec "$CHC" clickhouse-client -q 'select 1' >/dev/null 2>&1 && break; sleep 1; done
	chq() { docker exec -i "$CHC" clickhouse-client --max_threads 3 "$@"; }
	chq -n <<-'EOF'
		CREATE TABLE facts_ins (id UInt64, dim_a_id UInt32, foreign_id UInt8, lang_id UInt8, signal UInt8, fact_date Date, score Float64, deleted_at Nullable(DateTime)) ENGINE = MergeTree ORDER BY id;
		CREATE TABLE facts_tuned (id UInt64, dim_a_id UInt32, foreign_id UInt8, lang_id UInt8, signal UInt8, fact_date Date, score Float64, deleted_at Nullable(DateTime))
			ENGINE = MergeTree PARTITION BY toYYYYMM(fact_date) ORDER BY (foreign_id, dim_a_id, fact_date, id);
		CREATE TABLE memberships (group_id UInt32, dim_a_id UInt32, deleted_at Nullable(DateTime)) ENGINE = MergeTree ORDER BY (group_id, dim_a_id);
	EOF
	copy_out() { sq -d sets -c "\\copy ($1) to stdout with (format csv, null '\\N')"; }
	copy_out "SELECT id, dim_a_id, foreign_id, lang_id, signal, fact_date, score, to_char(deleted_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS') FROM facts ORDER BY id" |
		chq --query 'INSERT INTO facts_ins FORMAT CSV'
	copy_out "SELECT group_id, dim_a_id, to_char(deleted_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS') FROM memberships" | chq --query 'INSERT INTO memberships FORMAT CSV'
	chq --query 'INSERT INTO facts_tuned SELECT * FROM facts_ins'
	chq -n --query 'OPTIMIZE TABLE facts_ins FINAL; OPTIMIZE TABLE facts_tuned FINAL'
fi

meta=(--meta "date=$(date -u +%Y-%m-%d)" --meta "cpu=$(lscpu | awk -F: '/Model name/ {gsub(/^ +/, "", $2); n=$2} /^CPU\(s\)/ {gsub(/ /, "", $2); t=$2} END {printf "%s (%s threads)", n, t}'), scope 3 CPUs 12G" \
	--meta "postgresql=PostgreSQL $(sq -d sets -c 'SHOW server_version')" --meta "build=$(git -C "$SIFT_BUILD" describe --tags --always)" \
	--meta "clickhouse=$([[ -n $CH ]] && docker exec "$CHC" clickhouse-client -q 'select version()' || echo none)" --meta "shared_buffers=$(sq -d sets -c 'SHOW shared_buffers')")
python3 -I "$HERE/sets.py" run --db sets --out "$OUT" --reps "$REPS" ${CH:+--ch "$CHC"} "${meta[@]}"
echo "sets: $OUT" >&2
if [[ -n $DOCS ]]; then
	cp "$OUT" "$REPO/bench/results/sets.tsv"
	python3 -I "$HERE/extras.py" render
fi
