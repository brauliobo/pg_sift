#!/usr/bin/env bash
# Where do concurrent writers of a sift index wait? Runs bulk INSERT .. SELECT in CLIENTS sessions against the
# cluster of the PG* environment (eval "$(scripts/pgtest.sh env)", release or testing flavor) and samples,
# 100 times, the wait events of the inserting backends and the page locks of the index:
# page 0 is the maintenance lock, page 2 the active-segment lock; BufferContent is a buffer lock (the metapage
# among them).
#
#   scripts/wait-profile.sh [CLIENTS=8] [ROWS_PER_CLIENT=20000] [WORDS=150]
set -euo pipefail

CLIENTS=${1:-8} ROWS=${2:-20000} WORDS=${3:-150}
DIR=$(mktemp -d /tmp/sift-wait-profile.XXXXXX)
trap 'rm -rf "$DIR"' EXIT
psql -X -Atq -v ON_ERROR_STOP=1 -c "DROP TABLE IF EXISTS t" -c "CREATE TABLE t (id serial, body text)" \
	-c "CREATE INDEX t_idx ON t USING sift (body)" >/dev/null
echo "INSERT INTO t (body) SELECT (SELECT string_agg('w' || floor(power(random(), 3) * 50000)::int, ' ')
	FROM generate_series(1, $WORDS)) FROM generate_series(1, $ROWS);" >"$DIR/bulk.sql"
pgbench -n -c "$CLIENTS" -j "$CLIENTS" -t 6 -f "$DIR/bulk.sql" >"$DIR/pgbench.out" 2>&1 &
PB=$!
psql -X -Atq -v ON_ERROR_STOP=1 -c "CREATE TEMP TABLE w (ev text, pg int, granted bool)" \
	-c "DO \$\$ BEGIN FOR i IN 1..100 LOOP
		INSERT INTO w SELECT coalesce(wait_event_type || ':' || wait_event, 'cpu'), NULL, NULL FROM pg_stat_activity
			WHERE query LIKE 'INSERT INTO t%' AND pid <> pg_backend_pid();
		INSERT INTO w SELECT 'page lock', page, granted FROM pg_locks WHERE locktype = 'page';
		PERFORM pg_sleep(0.03); END LOOP; END \$\$" \
	-c "SELECT ev, count(*) AS samples FROM w WHERE ev <> 'page lock' GROUP BY 1 ORDER BY 2 DESC" \
	-c "SELECT 'page ' || pg || CASE WHEN granted THEN ' held' ELSE ' waited' END, count(*) FROM w WHERE ev = 'page lock' GROUP BY pg, granted ORDER BY 2 DESC"
wait "$PB"
