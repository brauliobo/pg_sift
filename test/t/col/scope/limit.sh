#!/usr/bin/env bash
# tap: pooled
# The multiplicity limit of a scope is a property of the index: CREATE INDEX records the limit it accepted the scope under in the option (max_multiplicity(n)), so a REINDEX, a restore of
# a dump and the rebuild after pg_upgrade (which run in a session with the default setting) check that limit, not their session's, and no build is paid for a refusal. A declaration may state it.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 13
node_init sl 56 "autovacuum = off"
node_start sl || bail "start failed"
D=$(dir sl)
S=("-h" "$(node_sock sl)" "-p" "$(port sl)" "-U" "postgres")
export PGOPTIONS="-c client_min_messages=warning"
dbq() { local db=$1; shift; psql -X -At -q -v ON_ERROR_STOP=1 "${S[@]}" -d "$db" "${@/#/--command=}"; }
errors() { grep -c 'ERROR:\|pg_restore: error' || true; }
fresh() { dbq "$(node_db sl)" "DROP DATABASE IF EXISTS $1" "CREATE DATABASE $1" >/dev/null; }
OPT="SELECT string_agg(o, ' ') FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 'ce_col' AND o LIKE 'scope=%'"
# 5 keys for each member of 200: the entries a row are 5, over the default limit of 3
SETUP=("CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test"
	"CREATE TABLE cp (m int NOT NULL, t int NOT NULL) WITH (autovacuum_enabled = false)" "CREATE UNIQUE INDEX cp_u ON cp (m, t)"
	"INSERT INTO cp SELECT m, 1 + (m + j * 3) % 16 FROM generate_series(1, 200) m, generate_series(0, 4) j"
	"CREATE INDEX cp_j ON cp USING sift_col (m, t) WITH (track_changes = 'ctid')"
	"CREATE TABLE ce (id int PRIMARY KEY, m int, d timestamptz, v int) WITH (autovacuum_enabled = false)"
	"INSERT INTO ce SELECT i, 1 + (i * 7919) % 200, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10 FROM generate_series(1, 20000) i"
	"VACUUM (FREEZE, ANALYZE) ce, cp")
DECL="t: m -> cp(m, t)"
refused() { dbq "$1" "$2" 2>&1 | grep -c "is over the limit of $3 entries a row"; }

fresh src
dbq src "${SETUP[@]}" >/dev/null || bail "setup failed"
check "a scope of 5 entries a row is refused by the default limit" test "$(refused src "CREATE INDEX ce_col ON ce USING sift_col (id, m, d, v) WITH (scope = '$DECL')" 3)" == 1
out=$(dbq src "CREATE INDEX ce_col ON ce USING sift_col (id, m, d, v) WITH (scope = '$DECL max_multiplicity(8)')" "$OPT" 2>&1)
check "with the limit declared it is accepted, and the option records it" test "$(dbq src "$OPT")" == "scope=$DECL max_multiplicity(8)"
check "REINDEX is accepted under the recorded limit" test "$(dbq src "REINDEX INDEX ce_col" 2>&1 | errors)" == 0 -a "$(dbq src "$OPT")" == "scope=$DECL max_multiplicity(8)"
check "the reads of the rebuilt index are the join's" test "$(dbq src "SET sift.agg_force = on" "SELECT count(*), sum(v) FROM ce e WHERE sift.in_scope('ce_col'::regclass, 't', e.m, 5)" | tail -n 1)" == \
	"$(dbq src "SET sift.enable_agg = off" "SELECT count(*), sum(v) FROM ce e WHERE e.m IN (SELECT m FROM cp WHERE t = 5)" | tail -n 1)"
dbq src "ALTER INDEX ce_col SET (scope = '$DECL max_multiplicity(2)')" >/dev/null
check "the limit the declaration states is the limit: the REINDEX is refused at 2" test "$(refused src "REINDEX INDEX ce_col" 2)" == 1
dbq src "ALTER INDEX ce_col SET (scope = '$DECL max_multiplicity(6.5)')" >/dev/null
check "... and a limit that allows the scope is kept by the REINDEX" test "$(dbq src "REINDEX INDEX ce_col" 2>&1 | errors)" == 0 -a "$(dbq src "$OPT")" == "scope=$DECL max_multiplicity(6.5)"

pg_dump "${S[@]}" -Fc -f "$D/src.custom" src
pg_dump "${S[@]}" -Fp -f "$D/src.sql" src
check "pg_dump writes the limit in the option" test "$(grep -c "scope='$DECL max_multiplicity(6.5)'" "$D/src.sql")" == 1
fresh r1
check "pg_restore makes the index under the recorded limit" test "$(pg_restore "${S[@]}" -d r1 "$D/src.custom" 2>&1 | errors)" == 0 -a "$(dbq r1 "$OPT")" == "scope=$DECL max_multiplicity(6.5)"
fresh r2
check "pg_dump | psql, the same" test "$(psql -X -q "${S[@]}" -d r2 -f "$D/src.sql" 2>&1 | errors)" == 0 -a "$(dbq r2 "$OPT")" == "scope=$DECL max_multiplicity(6.5)"
check "REINDEX of the restored index is accepted too" test "$(dbq r2 "REINDEX INDEX ce_col" 2>&1 | errors)" == 0

# an unlimited declaration of a second scope: each scope of an index has its own limit
dbq src "DROP INDEX ce_col" "CREATE TABLE cq (m int NOT NULL, g int NOT NULL, UNIQUE (m, g))" "INSERT INTO cq SELECT m, 1 + m % 7 FROM generate_series(1, 200) m" \
	"CREATE INDEX cq_j ON cq USING sift_col (m, g) WITH (track_changes = 'ctid')" >/dev/null
check "a scope of 1 entry a row is accepted beside one of 5 that states its limit, each with its own" \
	test "$(dbq src "CREATE INDEX ce_col ON ce USING sift_col (id, m, d, v) WITH (scope = '$DECL max_multiplicity(5); g: m -> cq(m, g)')" "$OPT" 2>&1 | grep -c ERROR)" == 0 \
	-a "$(dbq src "$OPT")" == "scope=$DECL max_multiplicity(5); g: m -> cq(m, g) max_multiplicity(3)"
bad() { dbq src "CREATE INDEX ce_bad ON ce USING sift_col (id, m, d, v) WITH (scope = '$DECL $1')" 2>&1 | grep -c "max_multiplicity"; }
check "a limit of 0, a text or a second one in a declaration is a syntax error" test "$(bad 'max_multiplicity(0)')" -ge 1 -a "$(bad 'max_multiplicity(x)')" -ge 1 -a "$(bad 'max_multiplicity(4) max_multiplicity(5)')" -ge 1
check "the indexes verify" test -z "$(dbq src "SELECT sift.col_verify('ce_col', true)" 2>&1)" -a -z "$(dbq r1 "SELECT sift.col_verify('ce_col', true)" 2>&1)"
finish
