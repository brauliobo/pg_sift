#!/usr/bin/env bash
# A sift_table through schema changes, bulk loads, wide values and unique checks: a COPY keeps the deletes its transaction staged before it, an empty string default and a numeric(p,s)
# widened in place leave the parts readable and compactable, ALTER TABLE on a partitioned parent reaches its sift_table partitions, a wide value outlives an ADD COLUMN, a concurrent
# UPDATE or INSERT that would duplicate a key of an expression, partial or sift UNIQUE index waits and fails, and an AFTER ROW trigger reads the rows of one long transaction without
# writing a part for each.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 11
node_init s105 2 "autovacuum = off
shared_buffers = 32MB
max_parallel_workers_per_gather = 0
${SIFT_TESTING:+sift.test_create_format1 = off}"
node_start s105 || bail "start failed"
D=$(dir s105)
sql s105 "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"
q() { sql s105 "$@" 2>&1 | paste -sd, -; }

sql s105 "CREATE TABLE cp (id int, v text) USING sift_table" "INSERT INTO cp SELECT g, 'x' FROM generate_series(1, 10) g" \
	"COPY (SELECT g, 'y' FROM generate_series(11, 20) g) TO '$D/cp.csv' CSV" >/dev/null
out=$(q "BEGIN; DELETE FROM cp WHERE id = 3; COPY cp FROM '$D/cp.csv' CSV; COMMIT" "SELECT count(*) || ':' || count(*) FILTER (WHERE id = 3) FROM cp")
check "a COPY keeps the delete its transaction staged before it" test "$out" == "19:0"

sql s105 "CREATE TABLE de (id int) USING sift_table" "INSERT INTO de SELECT generate_series(1, 5)" "ALTER TABLE de ADD COLUMN c text DEFAULT ''" \
	"INSERT INTO de VALUES (6)" >/dev/null
out=$(q "SELECT count(*) FILTER (WHERE c = '') FROM de" "VACUUM FULL de" "SELECT count(*) FILTER (WHERE c = '') FROM de")
check "an empty string default reads and survives VACUUM FULL" test "$out" == "6,6"

sql s105 "CREATE TABLE wd (id int) USING sift_table" "INSERT INTO wd SELECT generate_series(1, 5)" "ALTER TABLE wd ADD COLUMN n numeric(10,2) DEFAULT 1.5" \
	"INSERT INTO wd VALUES (6)" "ALTER TABLE wd ALTER COLUMN n TYPE numeric" "INSERT INTO wd VALUES (7, 2.25)" >/dev/null
out=$(q "VACUUM wd" "SELECT string_agg(n::text, ' ' ORDER BY id) FROM wd")
check "a numeric(p,s) default widened in place compacts" test "$out" == "1.50 1.50 1.50 1.50 1.50 1.50 2.25"

sql s105 "CREATE TABLE pt (id int, v text) PARTITION BY RANGE (id)" "CREATE TABLE pt_a PARTITION OF pt FOR VALUES FROM (0) TO (1000) USING sift_table" \
	"CREATE TABLE pt_b PARTITION OF pt FOR VALUES FROM (1000) TO (2000) USING sift_table" "INSERT INTO pt SELECT g, 'v' || g FROM generate_series(1, 1999) g" \
	"ALTER TABLE pt ADD COLUMN b int DEFAULT 42" "INSERT INTO pt VALUES (5, 'z', 7)" "ALTER TABLE pt DROP COLUMN v" "ALTER TABLE pt RENAME COLUMN b TO bb" >/dev/null
out=$(q "SELECT count(*) || ':' || sum(bb) FROM pt" "VACUUM pt_a, pt_b" "SELECT count(*) || ':' || sum(bb) FROM pt" "SELECT count(*) FROM sift.check('pt_a')")
check "ALTER TABLE of a partitioned parent changes its sift_table partitions" test "$out" == "2000:83965,2000:83965,0"

sql s105 "CREATE TABLE bl (id int, w text) USING sift_table" \
	"INSERT INTO bl SELECT g, CASE WHEN g % 50 = 0 THEN (SELECT string_agg(md5((g * 1000 + k)::text), '') FROM generate_series(1, 1000) k) ELSE 'n' END FROM generate_series(1, 2000) g" \
	"VACUUM bl" "ALTER TABLE bl ADD COLUMN t text" "DELETE FROM bl WHERE id % 100 = 0" >/dev/null
out=$(q "VACUUM bl" "SELECT count(*) || ':' || count(*) FILTER (WHERE length(w) = 32000) FROM bl" "SELECT count(*) FROM sift.check('bl')")
check "the wide values of a table outlive an ADD COLUMN and its check is clean" test "$out" == "1980:20,0"

# dup NAME INDEX FIRST: the second session inserts the key 9 while the first, in a transaction, runs FIRST that makes it; the second waits and fails, and no key is there twice
dup() {
	sql s105 "CREATE TABLE $1 (id int, k int) USING sift_table" "$2" "INSERT INTO $1 VALUES (1, 1), (2, 2)" >/dev/null
	race s105 "$3" "INSERT INTO $1 VALUES (3, 9)"
	[[ $RACE_WAITED == 1 ]] && grep -q 'duplicate key' "$D/rb.out" && [[ $(sql s105 "SELECT count(*) FROM $1 WHERE k = 9") == 1 ]]
}
check "an UPDATE of an expression UNIQUE key blocks a concurrent duplicate" dup ux "CREATE UNIQUE INDEX ON ux ((k + 0))" "UPDATE ux SET k = 9 WHERE id = 1"
check "an UPDATE of a partial UNIQUE key blocks a concurrent duplicate" dup up "CREATE UNIQUE INDEX ON up (k) WHERE id > 0" "UPDATE up SET k = 9 WHERE id = 1"
check "an INSERT under UNIQUE USING sift blocks a concurrent duplicate" dup us "CREATE UNIQUE INDEX ON us USING sift (k)" "INSERT INTO us VALUES (5, 9)"

sql s105 "CREATE TABLE nw (id int, n numeric(10,2), pad text) USING sift_table" "INSERT INTO nw SELECT g, g / 4.0, 'p' FROM generate_series(1, 300) g" "VACUUM nw" \
	"UPDATE nw SET n = 7.25 WHERE id = 10" "ALTER TABLE nw ALTER COLUMN n TYPE numeric" >/dev/null
out=$(q "SELECT n FROM nw WHERE ctid = (SELECT ctid FROM nw WHERE id = 10)")
check "a row updated before its numeric(p,s) was widened reads its new value" test "$out" == "7.25"

sql s105 "CREATE TABLE tg (id int, v text) USING sift_table" "CREATE TABLE tgh (id int, v text)" \
	"CREATE FUNCTION nop() RETURNS trigger LANGUAGE plpgsql AS \$\$ BEGIN RETURN NULL; END \$\$" \
	"CREATE TRIGGER tg_after AFTER INSERT ON tg FOR EACH ROW EXECUTE FUNCTION nop()" >/dev/null
out=$(q "DO \$\$ BEGIN FOR i IN 1..2500 LOOP INSERT INTO tg VALUES (i, repeat('x', 1000) || i); INSERT INTO tgh VALUES (i, repeat('x', 1000) || i); END LOOP; END \$\$" \
	"SELECT count(*) FROM ((TABLE tg EXCEPT ALL TABLE tgh) UNION ALL (TABLE tgh EXCEPT ALL TABLE tg)) d")
check "2500 single-row INSERTs of one transaction under an AFTER ROW trigger" test "$out" == "0"

sql s105 "CREATE TABLE tk (id int PRIMARY KEY, g int NOT NULL) WITH (autovacuum_enabled = false)" "INSERT INTO tk SELECT i, i % 10 FROM generate_series(1, 3000) i" \
	"ALTER TABLE tk ADD COLUMN s text DEFAULT 'abc'" "CREATE INDEX tk_c ON tk USING sift_col (id, g, s) WITH (key_order = 's, id', track_changes = 'ctid')" \
	"DELETE FROM tk WHERE id IN (5, 6, 7)" >/dev/null 2>&1
out=$(q "SET enable_seqscan = off" "SELECT count(*) FROM tk WHERE s = 'abc'" "SELECT count(*) FROM sift.check('tk_c', true)")
check "a tracked delete reads a key column added with a fast default" test "$out" == "2997,0"
finish
