#!/usr/bin/env bash
# tap: pooled
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# Eight clients add one to a few hot rows of a sift_table with a primary key, through INSERT .. ON CONFLICT DO UPDATE and UPDATE: a row is held by one writer at a time, also after a
# committed column delta of the row is past every snapshot, so that ON CONFLICT updates the row it locked and no increment is lost. The table ends with one row a key and the sum of all commits.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 5
node_init oc 31 "autovacuum = off
max_connections = 100"
node_start oc || bail "start failed"
D=$(dir oc)
sql oc "CREATE EXTENSION IF NOT EXISTS pg_sift" \
	"CREATE TABLE hot (id int PRIMARY KEY, v bigint NOT NULL) USING sift_table" \
	"INSERT INTO hot SELECT g, 0 FROM generate_series(1, 25) g" >/dev/null || bail "setup failed"

cat >"$D/upsert.pgb" <<'EOF'
\set k random_zipfian(1, 50, 1.5)
INSERT INTO hot VALUES (:k, 1) ON CONFLICT (id) DO UPDATE SET v = hot.v + 1;
EOF
cat >"$D/update.pgb" <<'EOF'
\set k random_zipfian(1, 25, 1.5)
UPDATE hot SET v = v + 1 WHERE id = :k;
EOF

pgbench -n -c 8 -j 8 -t "$(knob ONCONFLICT_TX 1000 300)" -f "$D/upsert.pgb@3" -f "$D/update.pgb@1" \
	-h "$(node_sock oc)" -p "$(port oc)" -U postgres -d "$(node_db oc)" >"$D/bench.out" 2>&1
out=$(grep -E 'actually processed|failed transactions|error' "$D/bench.out")
DONE=$(sed -n 's|^number of transactions actually processed: \([0-9]*\)/.*|\1|p' "$D/bench.out")
check "every transaction of the clients commits" grep -q "^number of failed transactions: 0 " "$D/bench.out"
IFS='|' read -r ROWS KEYS SUM <<<"$(sql oc "SELECT count(*), count(DISTINCT id), sum(v) FROM hot")"
out="processed $DONE, rows $ROWS, keys $KEYS, sum $SUM"
check "the server answers after the clients" test -n "$SUM"
check "the sum of the rows is every committed increment" test "$SUM" == "$DONE"
check "one row a key" test "$ROWS" == "$KEYS"
check "sift.check finds nothing" test "$(sql oc "SELECT count(*) FROM sift.check('hot'::regclass)")" == 0
finish
