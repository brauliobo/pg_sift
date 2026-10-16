#!/usr/bin/env bash
# Facet counts (DESIGN.md (l)): sift.facets against GROUP BY count(*) on sift and on GIN, and an
# Elasticsearch terms aggregation, on Simple English Wikipedia with two derived columns:
#   lo  first letter of the article (27 values)           hi  hashed category (NCAT values, default 20000)
#
#   bench/steps/facets.sh
#
# Needs $ARTICLES (default /tmp/sift-bench/data/articles.tsv, bench/datasets/fetch.sh articles). Builds the
# production library into a scratch cluster under $FACET_TMP (default /tmp/sift-facets-bench, removed on
# exit unless KEEP=1). Three workloads of NQ queries each: `one` (a word of every document frequency
# band), `and` and `or` (two words). Every query is first checked: sift.facets must equal the sift GROUP
# BY (a difference fails the run; GIN tokenizes differently and only warns). Then each cell (column,
# workload, method) runs for SECS seconds with CLIENTS pgbench clients, ROUNDS rounds with the method order
# rotated, and the median queries per second is reported. ES=auto|0|1 adds Elasticsearch (the system
# package, one shard, single client). PARALLEL sets max_parallel_workers_per_gather for the GROUP BYs.
# Results and `uptime` before and after land in $DEST (default bench/out/facets/<time>).
set -euo pipefail
export LC_ALL=C
STEPS=$(cd "$(dirname "$0")" && pwd)
BENCH=$(dirname "$STEPS")
REPO=$(dirname "$BENCH")
ARTICLES=${ARTICLES:-${SIFT_BENCH_ROOT:-/tmp/sift-bench}/data/articles.tsv}
TMP=${FACET_TMP:-/tmp/sift-facets-bench}
DOCS=${DOCS:-25000} SECS=${SECS:-3} ROUNDS=${ROUNDS:-2} CLIENTS=${CLIENTS:-1} NQ=${NQ:-24} NCAT=${NCAT:-2500}
PARALLEL=${PARALLEL:-2} ES=${ES:-auto} ES_HEAP=${ES_HEAP:-4g} ES_PORT=${ES_PORT:-56170}
DEST=${DEST:-$BENCH/out/facets/$(date +%m%d-%H%M)}
export SIFT_TMP=$TMP SIFT_PORT=${SIFT_PORT:-56162} SIFT_DB=facets SIFT_TESTING=
PT=$REPO/scripts/pgtest.sh
eval "$("$PT" env)"
VARIANTS=(facets sift_gb gin_gb)
[[ -f $ARTICLES ]] || { echo "no $ARTICLES: run bench/datasets/fetch.sh articles" >&2; exit 1; }

log() { printf '[%s] %s\n' "$(date +%T)" "$*" >&2; }
q() { "$PT" psql -qAt -v ON_ERROR_STOP=1 "$@"; }
cleanup() {
	[[ -f $TMP/es/pid ]] && kill "$(cat "$TMP/es/pid")" 2>/dev/null || true
	[[ -n ${KEEP:-} ]] || "$PT" destroy >/dev/null 2>&1 || true
}
trap cleanup EXIT

build() {
	# the production library, built with the caller's job count instead of pgtest.sh's
	[[ $(cat "$REPO/.build-flavor" 2>/dev/null) == "" ]] || make -C "$REPO" USE_PGXS=1 clean >/dev/null
	nice make -C "$REPO" -j2 USE_PGXS=1 >/dev/null
	echo >"$REPO/.build-flavor"
	"$PT" install
	"$PT" start
	q -c "ALTER SYSTEM SET shared_buffers = '2GB'" -c "ALTER SYSTEM SET jit = off" -c "ALTER SYSTEM SET work_mem = '64MB'" \
		-c "ALTER SYSTEM SET maintenance_work_mem = '1GB'" -c "ALTER SYSTEM SET max_parallel_workers_per_gather = $PARALLEL" \
		-c "ALTER SYSTEM SET max_wal_size = '4GB'"
	"$PT" stop
	"$PT" start
}

load() {
	q -c "CREATE EXTENSION pg_sift"
	q <<-SQL
		CREATE UNLOGGED TABLE src (src_id bigint, body text, category_id int, created date, likes int, url text);
		\\copy src FROM PROGRAM 'head -n $DOCS $ARTICLES'
		CREATE TABLE docs AS SELECT src_id AS id, body,
			coalesce(nullif(left(lower(regexp_replace(body, '^[^[:alpha:]]+', '')), 1), ''), '_') AS lo,
			'c' || abs(hashint8(src_id)) % $NCAT AS hi, to_tsvector('simple', body) AS tsv FROM src;
		DROP TABLE src;
		CREATE INDEX docs_body ON docs USING sift (body);
		CREATE INDEX docs_lo ON docs USING sift (lo) WITH (analyzer = keyword);
		CREATE INDEX docs_hi ON docs USING sift (hi) WITH (analyzer = keyword);
		CREATE INDEX docs_tsv ON docs USING gin (tsv);
		VACUUM (ANALYZE) docs;
		CHECKPOINT;
	SQL
	q -c "SELECT format('%s docs, lo %s values, hi %s values; indexes: body %s, lo %s, hi %s, gin %s',
			(SELECT count(*) FROM docs), (SELECT count(DISTINCT lo) FROM docs), (SELECT count(DISTINCT hi) FROM docs),
			pg_size_pretty(pg_relation_size('docs_body')), pg_size_pretty(pg_relation_size('docs_lo')),
			pg_size_pretty(pg_relation_size('docs_hi')), pg_size_pretty(pg_relation_size('docs_tsv')))" >&2
}

queries() {
	q -v nq="$NQ" <<-'SQL'
		CREATE TABLE words AS SELECT word, ndoc FROM ts_stat('SELECT tsv FROM docs') WHERE word ~ '^[a-z]{4,12}$';
		CREATE TABLE qs (wl text, n int, mode text, words text);
		INSERT INTO qs SELECT 'one', row_number() OVER (ORDER BY s, r), 'or', word FROM (
			SELECT word, s, row_number() OVER (PARTITION BY s ORDER BY hashtext(word)) AS r
			FROM (SELECT word, width_bucket(ndoc, ARRAY[100, 1000, 5000, 30000]) AS s FROM words WHERE ndoc >= 100) x) y
			WHERE r <= :nq / 4;
		INSERT INTO qs SELECT 'and', pair + 1, 'and', string_agg(word, ' ' ORDER BY r) FROM (
			SELECT word, r, (r - 1) / 2 AS pair FROM (SELECT word, row_number() OVER (ORDER BY hashtext(word || 'and')) AS r
			FROM words WHERE ndoc BETWEEN 2000 AND 60000) x WHERE r <= 2 * :nq) y GROUP BY pair;
		INSERT INTO qs SELECT 'or', pair + 1, 'or', string_agg(word, ' ' ORDER BY r) FROM (
			SELECT word, r, (r - 1) / 2 AS pair FROM (SELECT word, row_number() OVER (ORDER BY hashtext(word || 'or')) AS r
			FROM words WHERE ndoc BETWEEN 300 AND 10000) x WHERE r <= 2 * :nq) y GROUP BY pair;
		CREATE TABLE stmts (wl text, col text, variant text, n int, stmt text);
		INSERT INTO stmts SELECT wl, col, variant, n, CASE variant
			WHEN 'facets'  THEN format('SELECT * FROM sift.facets(''docs_body'', ''docs_%s'', %L, 10)', col, sq)
			WHEN 'sift_gb' THEN format('SELECT %I, count(*) FROM docs WHERE body @@> %L GROUP BY 1 ORDER BY 2 DESC, 1 LIMIT 10', col, sq)
			WHEN 'gin_gb'  THEN format('SELECT %I, count(*) FROM docs WHERE tsv @@ to_tsquery(''simple'', %L) GROUP BY 1 ORDER BY 2 DESC, 1 LIMIT 10', col, tq)
			END
		FROM (SELECT wl, n, replace(words, ' ', CASE mode WHEN 'and' THEN ' AND ' ELSE ' OR ' END) AS sq,
				replace(words, ' ', CASE mode WHEN 'and' THEN ' & ' ELSE ' | ' END) AS tq FROM qs) x,
			unnest('{lo,hi}'::text[]) col, unnest('{facets,sift_gb,gin_gb}'::text[]) variant;
		CREATE FUNCTION res(stmt text) RETURNS text LANGUAGE plpgsql AS $f$
		DECLARE r text;
		BEGIN
			EXECUTE format('SELECT string_agg(a || ''='' || b, '' '' ORDER BY b::bigint DESC, a) FROM (%s) s(a, b)', stmt) INTO r;
			RETURN coalesce(r, '');
		END $f$;
		CREATE TABLE res (round int, col text, wl text, variant text, qps float8);
	SQL
}

# Every facet result equals the sift GROUP BY; GIN may differ where its parser splits a word differently
check() {
	local out
	out=$(q -F ' ' -c "SELECT col, wl, count(*), count(*) FILTER (WHERE f IS DISTINCT FROM s), count(*) FILTER (WHERE f IS DISTINCT FROM g)
		FROM (SELECT a.col, a.wl, res(a.stmt) AS f, res(b.stmt) AS s, res(c.stmt) AS g FROM stmts a
			JOIN stmts b ON (b.col, b.wl, b.n) = (a.col, a.wl, a.n) AND b.variant = 'sift_gb'
			JOIN stmts c ON (c.col, c.wl, c.n) = (a.col, a.wl, a.n) AND c.variant = 'gin_gb' WHERE a.variant = 'facets') x
		GROUP BY 1, 2 ORDER BY 1, 2")
	log "check (column, workload, queries, facets != sift group by, facets != gin group by):"
	echo "$out" | sed 's/^/  /' >&2
	[[ -z $(awk '$4 > 0' <<<"$out") ]] || { echo "sift.facets differs from GROUP BY" >&2; exit 1; }
}

plans() {
	local v
	for v in "${VARIANTS[@]}"; do
		log "plan $v (hi, and #1):"
		q -c "SELECT 'EXPLAIN (COSTS OFF) ' || stmt FROM stmts WHERE wl = 'and' AND col = 'hi' AND variant = '$v' AND n = 1" |
			"$PT" psql -qAt | sed 's/^/  /' >&2
	done
}

cell() { # round col wl variant
	local f=$TMP/cell.pgb n tps
	n=$(q -c "SELECT count(*) FROM stmts WHERE wl = '$3' AND col = '$2' AND variant = '$4'")
	printf '\\set i random(1, %d)\nSELECT stmt FROM stmts WHERE wl = '"'%s'"' AND col = '"'%s'"' AND variant = '"'%s'"' AND n = :i \\gset\n:stmt\n' \
		"$n" "$3" "$2" "$4" >"$f"
	pgbench -n -f "$f" -c "$CLIENTS" -j "$CLIENTS" -T 1 facets >/dev/null
	tps=$(pgbench -n -f "$f" -c "$CLIENTS" -j "$CLIENTS" -T "$SECS" facets | sed -n 's/^tps = \([0-9.]*\) .*/\1/p')
	q -c "INSERT INTO res VALUES ($1, '$2', '$3', '$4', $tps)"
}

measure() {
	local r col wl i v
	for (( r = 1; r <= ROUNDS; r++ )); do
		log "round $r"
		for col in lo hi; do for wl in one and or; do
			for (( i = 0; i < ${#VARIANTS[@]}; i++ )); do
				cell "$r" "$col" "$wl" "${VARIANTS[(i + r) % ${#VARIANTS[@]}]}"
			done
		done; done
	done
}

es_conf() {
	local d=$TMP/es
	mkdir -p "$d/conf" "$d/data" "$d/logs" "$d/tmp"
	cat >"$d/conf/elasticsearch.yml" <<-EOF
		cluster.name: facets
		path.data: $d/data
		path.logs: $d/logs
		http.port: $ES_PORT
		transport.port: $((ES_PORT + 1))
		network.host: 127.0.0.1
		discovery.type: single-node
		xpack.security.enabled: false
		cluster.routing.allocation.disk.threshold_enabled: false
	EOF
	printf -- '-Xms%s\n-Xmx%s\n-XX:-AlwaysPreTouch\n-Djava.io.tmpdir=%s\n' "$ES_HEAP" "$ES_HEAP" "$d/tmp" >"$d/conf/jvm.options"
	cat >"$d/conf/log4j2.properties" <<-'EOF'
		status = error
		appender.console.type = Console
		appender.console.name = console
		appender.console.layout.type = PatternLayout
		appender.console.layout.pattern = [%d{ISO8601}][%-5p][%-25c{1.}] %m%n
		rootLogger.level = info
		rootLogger.appenderRef.console.ref = console
	EOF
}

elasticsearch() {
	local d=$TMP/es url=http://127.0.0.1:$ES_PORT
	es_conf
	q -F $'\t' -c "SELECT wl, n, mode, words FROM qs ORDER BY wl, n" >"$d/queries.tsv"
	q -v FETCH_COUNT=20000 -c "SELECT json_build_object('id', id::text, 'body', body, 'lo', lo, 'hi', hi) FROM docs ORDER BY id" >"$d/docs.ndjson"
	ES_PATH_CONF=$d/conf ES_TMPDIR=$d/tmp nice /usr/share/elasticsearch/bin/elasticsearch -d -p "$d/pid" >"$d/start.log" 2>&1
	until curl -fs "$url/_cluster/health?wait_for_status=yellow" >/dev/null; do sleep 1; done
	log "elasticsearch up"
	python3 -I "$STEPS/facets_es.py" "$url" "$d/docs.ndjson" "$d/queries.tsv" "$SECS" "$ROUNDS" >"$d/es.out"
	awk -F'\t' '$1 == "check"' "$d/es.out" | sed 's/^/  /' >&2
	awk -F'\t' '$1 != "check" {print $1 "\t" $2 "\t" $3}' "$d/es.out" | awk -F'\t' '{r[$1 FS $2]++; print r[$1 FS $2] "\t" $0}' |
		while IFS=$'\t' read -r r col wl qps; do q -c "INSERT INTO res VALUES ($r, '$col', '$wl', 'es', $qps)"; done
	kill "$(cat "$d/pid")"; while [[ -e $d/pid ]]; do sleep 1; done
}

summary() {
	q -P footer=off -P format=aligned -c "SELECT col, wl, round(max(qps) FILTER (WHERE variant = 'facets')) AS facets,
			round(max(qps) FILTER (WHERE variant = 'sift_gb')) AS sift_group_by, round(max(qps) FILTER (WHERE variant = 'gin_gb')) AS gin_group_by,
			round(max(qps) FILTER (WHERE variant = 'es')) AS es_terms,
			round((max(qps) FILTER (WHERE variant = 'facets') / max(qps) FILTER (WHERE variant = 'sift_gb'))::numeric, 2) AS x_sift,
			round((max(qps) FILTER (WHERE variant = 'facets') / max(qps) FILTER (WHERE variant = 'gin_gb'))::numeric, 2) AS x_gin
		FROM (SELECT col, wl, variant, percentile_cont(0.5) WITHIN GROUP (ORDER BY qps) AS qps FROM res GROUP BY 1, 2, 3) m
		GROUP BY col, wl ORDER BY col DESC, wl"
}

mkdir -p "$DEST"
{
	echo "uptime before: $(uptime)"
	echo "settings: DOCS=$DOCS SECS=$SECS ROUNDS=$ROUNDS CLIENTS=$CLIENTS NQ=$NQ NCAT=$NCAT PARALLEL=$PARALLEL; median qps of $ROUNDS rounds"
} | tee "$DEST/summary.txt" >&2
build
load
queries
check
plans | tee -a "$DEST/summary.txt"
measure
[[ $ES == 0 ]] || [[ $ES == auto && ! -x /usr/share/elasticsearch/bin/elasticsearch ]] || elasticsearch || log "elasticsearch failed: see $TMP/es"
{
	echo "uptime after: $(uptime)"
	summary
} | tee -a "$DEST/summary.txt"
